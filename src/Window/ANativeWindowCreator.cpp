#include "Window/ANativeWindowCreator.h"

#include "Utils/Logger.h"
#include "Utils/System.h"
#include "Window/LibGui.h"
#include "Window/Mirror.h"

#include <android/native_window.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <format>
#include <mutex>
#include <pthread.h>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <utility>

static constexpr uint32_t MainStack = 0;

// Keeps the layer mirrored on every other display, from a thread of its own: asking the
// system which displays exist takes milliseconds, which a frame cannot spare. The system is
// asked once a second, and only while there is a layer to mirror.
class MirrorWorker {
public:
    // Nothing where recordings see the layer without help, or mirroring is missing.
    static std::unique_ptr<MirrorWorker> Start(const LibGui& gui, GuiHandle client) {
        // Recordings read displays of their own from Android 14 on.
        if (gui.GetApi() < 34 || !gui.CanMirror()) {
            return nullptr;
        }
        return std::unique_ptr<MirrorWorker>(new MirrorWorker(gui, std::move(client)));
    }

    // Sets the layer to mirror and its size, or nothing to mirror nothing.
    void Follow(std::optional<std::pair<GuiHandle, std::array<uint32_t, 2>>> layer) {
        {
            const std::scoped_lock lock(mutex_);
            version_++;
            if (layer) {
                source_ = Source{std::move(layer->first), layer->second, version_};
            } else {
                source_.reset();
            }
        }
        wake_.notify_one();
    }

private:
    struct Source {
        GuiHandle layer;
        std::array<uint32_t, 2> size{};
        uint64_t version = 0;
    };

    struct Copy {
        GuiHandle root;
        GuiHandle copy;
        std::optional<MirrorPlacement> placed;
    };

    MirrorWorker(const LibGui& gui, GuiHandle client) : gui_(gui), client_(std::move(client)) {
        worker_ = std::jthread([this](std::stop_token stop) {
            pthread_setname_np(pthread_self(), "mirror");
            Work(stop);
        });
    }

    void Work(std::stop_token stop) {
        const auto transaction = gui_.NewTransaction();
        if (!transaction) {
            return;
        }
        std::unordered_map<uint32_t, Copy> copies;
        uint64_t version = 0;
        while (!stop.stop_requested()) {
            std::optional<Source> source;
            {
                std::unique_lock lock(mutex_);
                if (!source_) {
                    // Nothing to mirror, so nothing to watch for either, until there is.
                    copies.clear();
                    wake_.wait(lock, stop, [&] { return source_.has_value(); });
                    continue;
                }
                source = source_;
            }
            if (source->version != version) {
                copies.clear();
                version = source->version;
            }
            const std::vector<ShownDisplay> shown = Mirror::ParseDisplays(System::RunCommand("dumpsys display 2>/dev/null"));
            std::erase_if(copies, [&](const auto& entry) { return std::ranges::none_of(shown, [&](const ShownDisplay& display) { return display.stack == entry.first; }); });

            const auto main = std::ranges::find(shown, MainStack, &ShownDisplay::stack);
            for (const ShownDisplay& display : shown) {
                if (display.stack == MainStack) {
                    continue;
                }
                if (main == shown.end()) {
                    break;
                }
                if (!copies.contains(display.stack)) {
                    auto mirrored = gui_.Mirror(client_.Get(), *transaction, source->layer.Get(), source->size, display.stack);
                    if (mirrored) {
                        logger.Info("mirroring the overlay to display layer stack {}", display.stack);
                        copies.emplace(display.stack, Copy{std::move((*mirrored)[1]), std::move((*mirrored)[0]), std::nullopt});
                    } else {
                        logger.Warn("cannot mirror to layer stack {}: {}", display.stack, mirrored.error().message);
                    }
                }
                const MirrorPlacement placement = Mirror::Fit(main->size, display.size);
                if (const auto copy = copies.find(display.stack); copy != copies.end() && copy->second.placed != placement) {
                    if (const auto placed = gui_.Place(*transaction, copy->second.root.Get(), placement.scale, placement.offset)) {
                        copy->second.placed = placement;
                    } else {
                        logger.Warn("cannot place the mirror on layer stack {}: {}", display.stack, placed.error().message);
                    }
                }
            }

            // The next look is a second away, or as soon as the layer to mirror changes.
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, stop, std::chrono::seconds(1), [&] { return !source_ || source_->version != version; });
        }
    }

    const LibGui& gui_;
    GuiHandle client_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::optional<Source> source_;
    uint64_t version_ = 0;
    // Last, so that it stops and joins before anything it uses goes.
    std::jthread worker_;
};

// A layer on the main display with its window.
struct WindowLayer {
    GuiPtr control = nullptr;
    GuiPtr surface = nullptr;
    ANativeWindow* window = nullptr;
};

struct NativeWindow::State {
    const LibGui* gui = nullptr;
    GuiPtr client = nullptr;
    GuiPtr token = nullptr;
    GuiPtr transaction = nullptr;
    std::optional<uint64_t> displayId;
    std::optional<std::chrono::nanoseconds> reportedPeriod;

    std::string name;
    DisplayInfo display;
    Capture capture = Capture::Visible;
    WindowLayer layer;
    // The layer `Recreate` replaced, kept until whatever drew into it has let go.
    WindowLayer retired;
    uint64_t generation = 0;
    bool visible = true;
    bool mirroring = false;
    std::string error;
    std::unique_ptr<MirrorWorker> mirrors;

    ~State() {
        mirrors.reset();
        Drop(retired);
        Drop(layer);
    }

    // The window inside an `android::Surface`, with a reference of its own taken on it.
    static Result<ANativeWindow*> AcquireWindow(GuiPtr surface) {
        // An `android::Surface` is a reference-counted object with the window as its second base.
        auto* window = reinterpret_cast<ANativeWindow*>(static_cast<char*>(surface) + 2 * sizeof(void*));
        uint32_t magic = 0;
        std::memcpy(&magic, window, sizeof magic);
        // `android_native_base_t::magic` of a window: '_wnd'.
        if (magic != 0x5f776e64) {
            return Err("android::Surface has an unexpected layout on this system");
        }
        ANativeWindow_acquire(window);
        return window;
    }

    // The display's vsync period, tracked through the NDK choreographer (Android 11+).
    static std::optional<std::chrono::nanoseconds> ChoreographerPeriod() {
        using Prepare = void* (*)(int);
        using PollOnce = int (*)(int, int*, int*, void**);
        using GetInstance = void* (*)();
        using Callback = void (*)(int64_t, void*);
        using Register = void (*)(void*, Callback, void*);

        static std::atomic<uint64_t> periodNanos{0};
        // The thread that listens lives for as long as the process does: a looper cannot be asked
        // to stop from outside.
        static const bool Started = [] {
            const auto android = Library::Open("libandroid.so");
            if (!android) {
                return false;
            }
            const auto prepare = android->Func<Prepare>("ALooper_prepare");
            const auto pollOnce = android->Func<PollOnce>("ALooper_pollOnce");
            const auto instance = android->Func<GetInstance>("AChoreographer_getInstance");
            const auto subscribe = android->Func<Register>("AChoreographer_registerRefreshRateCallback");
            if (!prepare || !pollOnce || !instance || !subscribe) {
                return false;
            }

            std::thread([=] {
                pthread_setname_np(pthread_self(), "vsync");
                prepare(0);
                void* choreographer = instance();
                if (!choreographer) {
                    return;
                }
                subscribe(choreographer, [](int64_t nanos, void*) { periodNanos.store(nanos > 0 ? static_cast<uint64_t>(nanos) : 0, std::memory_order_relaxed); }, nullptr);
                for (;;) {
                    pollOnce(-1, nullptr, nullptr, nullptr);
                }
            }).detach();
            return true;
        }();

        const uint64_t nanos = periodNanos.load(std::memory_order_relaxed);
        if (!Started || nanos == 0) {
            return std::nullopt;
        }
        return std::chrono::nanoseconds(nanos);
    }

    static Result<std::shared_ptr<State>> Connect() {
        auto state = std::make_shared<State>();
        state->gui = TRY(LibGui::Get());
        state->client = TRY(state->gui->Connect());
        std::tie(state->token, state->displayId) = TRY(state->gui->GetDisplayToken());
        state->transaction = TRY(state->gui->NewTransaction());
        return state;
    }

    Result<DisplayInfo> ReadDisplay() {
        const DisplayState now = TRY(gui->GetDisplayState(token));
        if (now.width == 0 || now.height == 0) {
            return Err("SurfaceFlinger reported an empty display");
        }
        reportedPeriod = now.refreshRate.transform([](float rate) { return std::chrono::nanoseconds(static_cast<int64_t>(1e9 / rate)); });
        return DisplayInfo{now.width, now.height, static_cast<Rotation>(now.rotation & 3), System::GetDensity()};
    }

    Result<WindowLayer> CreateLayer() const {
        WindowLayer made;
        made.control = TRY(gui->CreateSurface(client, name.c_str(), display.width, display.height, capture, false));
        const auto finished = [&]() -> Result<> {
            made.surface = TRY(gui->GetSurface(made.control));
            made.window = TRY(AcquireWindow(made.surface));
            TRY(SetSize(made.window, display.width, display.height));
            return gui->Present(transaction, made.control, INT32_MAX);
        }();
        if (!finished) {
            Drop(made);
            return std::unexpected(finished.error());
        }
        return made;
    }

    void Drop(WindowLayer& gone) const {
        if (!gone.control) {
            return;
        }
        (void)gui->SetVisible(transaction, gone.control, false);
        if (gone.window) {
            ANativeWindow_release(gone.window);
        }
        if (gone.surface) {
            gui->Release(gone.surface);
        }
        gui->Disconnect(gone.control);
        gui->Release(gone.control);
        gone = {};
    }

    static Result<> SetSize(ANativeWindow* window, uint32_t width, uint32_t height) {
        const int32_t status = ANativeWindow_setBuffersGeometry(window, static_cast<int32_t>(width), static_cast<int32_t>(height), WINDOW_FORMAT_RGBA_8888);
        if (status != 0) {
            return Err("ANativeWindow_setBuffersGeometry failed (status {})", status);
        }
        return {};
    }

    // Recordings are shown the layer when asked to, for as long as it is not hidden from them.
    void MirrorLayer() const {
        if (!mirrors) {
            return;
        }
        if (capture == Capture::Visible) {
            mirrors->Follow(std::pair{GuiHandle::Share(*gui, layer.control), std::array{display.width, display.height}});
        } else {
            mirrors->Follow(std::nullopt);
        }
    }

    Result<> Recreate() {
        WindowLayer next = TRY(CreateLayer());
        Drop(retired);
        retired = std::exchange(layer, next);
        visible = true;
        generation++;
        MirrorLayer();
        return {};
    }
};

WindowBuffer::WindowBuffer(ANativeWindow* window, uint32_t* pixels, size_t width, size_t height, size_t stride) : window_(window), pixels_(pixels), width_(width), height_(height), stride_(stride) {
}

WindowBuffer::WindowBuffer(WindowBuffer&& other) noexcept : window_(std::exchange(other.window_, nullptr)), pixels_(other.pixels_), width_(other.width_), height_(other.height_), stride_(other.stride_) {
}

WindowBuffer::~WindowBuffer() {
    if (window_) {
        ANativeWindow_unlockAndPost(window_);
    }
}

std::span<uint32_t> WindowBuffer::GetRow(size_t y) {
    const std::span<uint32_t> pixels = GetPixels();
    return y < height_ && !pixels.empty() ? pixels.subspan(y * stride_, width_) : std::span<uint32_t>();
}

std::span<uint32_t> WindowBuffer::GetPixels() {
    if (height_ == 0 || width_ > stride_) {
        return {};
    }
    return {pixels_, (height_ - 1) * stride_ + width_};
}

NativeWindow::NativeWindow(std::shared_ptr<State> state) : state_(std::move(state)) {
}

ANativeWindow* NativeWindow::Get() const {
    return state_->layer.window;
}

uint64_t NativeWindow::GetGeneration() const {
    return state_->generation;
}

DisplayInfo NativeWindow::GetDisplayInfo() const {
    return state_->display;
}

std::optional<std::string> NativeWindow::GetDisplayId() const {
    return state_->displayId.transform([](uint64_t id) { return std::format("local:{}", id); });
}

uint32_t NativeWindow::GetApiLevel() const {
    return state_->gui->GetApi();
}

std::optional<std::chrono::nanoseconds> NativeWindow::GetVsyncPeriod() const {
    if (const auto period = State::ChoreographerPeriod()) {
        return period;
    }
    return state_->reportedPeriod;
}

std::vector<std::string> NativeWindow::GetSymbols() const {
    std::vector<std::string> lines;
    for (const SymbolPick& pick : state_->gui->GetPicks()) {
        lines.push_back(std::format("{}: {}", pick.what, pick.symbol));
    }
    return lines;
}

Capture NativeWindow::GetCapture() const {
    return state_->capture;
}

bool NativeWindow::SetCapture(Capture capture) {
    State& state = *state_;
    if (capture == state.capture) {
        return true;
    }
    const Capture before = std::exchange(state.capture, capture);
    if (const auto made = state.Recreate(); !made) {
        state.capture = before;
        state.error = made.error().message;
        logger.Error("{}", state.error);
        return false;
    }
    return true;
}

const std::string& NativeWindow::GetError() const {
    return state_->error;
}

bool NativeWindow::IsMirroring() const {
    return state_->mirroring;
}

void NativeWindow::SetMirroring(bool mirroring) {
    State& state = *state_;
    if (mirroring == state.mirroring) {
        return;
    }
    state.mirroring = mirroring;
    state.mirrors.reset();
    if (mirroring) {
        state.mirrors = MirrorWorker::Start(*state.gui, GuiHandle::Share(*state.gui, state.client));
    }
    state.MirrorLayer();
}

Result<> NativeWindow::SetVisible(bool visible) {
    State& state = *state_;
    if (visible == state.visible) {
        return {};
    }
    TRY(state.gui->SetVisible(state.transaction, state.layer.control, visible));
    state.visible = visible;
    return {};
}

Result<bool> NativeWindow::FollowDisplay() {
    State& state = *state_;
    const DisplayInfo current = TRY(state.ReadDisplay());
    if (current == state.display) {
        return false;
    }
    logger.Info("display is now {}x{} at {} degrees", current.width, current.height, current.GetDegrees());
    state.display = current;
    // Where this Android version cannot resize a layer in place, it is made again.
    if (TRY(state.gui->Resize(state.transaction, state.layer.control, current.width, current.height))) {
        TRY(State::SetSize(state.layer.window, current.width, current.height));
        state.generation++;
        state.MirrorLayer();
    } else {
        TRY(state.Recreate());
    }
    return true;
}

Result<> NativeWindow::Recreate() {
    return state_->Recreate();
}

void NativeWindow::ReleaseOld() {
    state_->Drop(state_->retired);
}

Result<WindowBuffer> NativeWindow::Lock() const {
    ANativeWindow* window = state_->layer.window;
    ANativeWindow_Buffer buffer{};
    const int32_t status = ANativeWindow_lock(window, &buffer, nullptr);
    if (status != 0 || !buffer.bits) {
        return Err("ANativeWindow_lock failed (status {})", status);
    }
    const auto side = [](int32_t value) { return value > 0 ? static_cast<size_t>(value) : 0; };
    return WindowBuffer(window, static_cast<uint32_t*>(buffer.bits), side(buffer.width), side(buffer.height), side(buffer.stride));
}

std::string ANativeWindowCreator::error_;

DisplayInfo ANativeWindowCreator::GetDisplayInfo() {
    const auto read = NativeWindow::State::Connect().and_then([](const std::shared_ptr<NativeWindow::State>& state) { return state->ReadDisplay(); });
    if (!read) {
        error_ = read.error().message;
        logger.Error("{}", error_);
        return {};
    }
    return *read;
}

NativeWindow ANativeWindowCreator::Create(std::string_view name, Capture capture) {
    const auto made = [&]() -> Result<NativeWindow> {
        const auto state = TRY(NativeWindow::State::Connect());
        state->name = name;
        state->capture = capture;
        state->display = TRY(state->ReadDisplay());
        state->layer = TRY(state->CreateLayer());
        return NativeWindow(state);
    }();
    if (!made) {
        error_ = made.error().message;
        logger.Error("{}", error_);
        return {};
    }
    return *made;
}

const std::string& ANativeWindowCreator::GetError() {
    return error_;
}
