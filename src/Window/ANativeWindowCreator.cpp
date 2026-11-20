#include "Window/ANativeWindowCreator.h"

#include "Utils/Logger.h"
#include "Utils/System.h"
#include "Window/LibGui.h"
#include "Window/Mirror.h"

#include <algorithm>
#include <android/native_window.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <format>
#include <limits>
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
    // Whether the window's next buffers carry a moment to be shown at.
    bool stamped = false;
    // The layer the blur is asked on, once any was asked for.
    GuiHandle blur;
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
    // The parts of the layer the system blurs under, kept for the layer that replaces this one.
    std::vector<BlurRegion> blur;
    std::string error;
    std::unique_ptr<MirrorWorker> mirrors;

    ~State() {
        mirrors.reset();
        Drop(retired);
        Drop(layer);
    }

    // The start of an `ANativeWindow` as the system lays it out, up to the call that changes
    // what its next buffers carry.
    struct WindowCalls {
        int32_t magic;
        int32_t version;
        void* reserved[4];
        void* incRef;
        void* decRef;
        uint32_t flags;
        int32_t minSwapInterval;
        int32_t maxSwapInterval;
        float xdpi;
        float ydpi;
        intptr_t oem[4];
        void* setSwapInterval;
        void* dequeueOld;
        void* lockOld;
        void* queueOld;
        void* query;
        int (*perform)(ANativeWindow*, int, ...);
    };

    // The moment the window's next buffers ask to be shown at; none, as soon as they can be.
    void Stamp(std::optional<std::chrono::steady_clock::time_point> shownAt) {
        if (!shownAt && !layer.stamped) {
            return;
        }
        // `NATIVE_WINDOW_SET_BUFFERS_TIMESTAMP`, with `NATIVE_WINDOW_TIMESTAMP_AUTO` for none.
        const int64_t nanos = shownAt ? std::chrono::duration_cast<std::chrono::nanoseconds>(shownAt->time_since_epoch()).count() : std::numeric_limits<int64_t>::min();
        reinterpret_cast<const WindowCalls*>(layer.window)->perform(layer.window, 7, nanos);
        layer.stamped = shownAt.has_value();
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

    // The display's own rhythm, asked of the NDK's choreographer (Android 7 and later). To a
    // program without a Java machine the choreographer says nothing by itself, so it is asked
    // to call at a few refreshes in a row, a few times a second for as long as frames go on.
    struct Rhythm {
        using Prepare = void* (*)(int);
        using PollOnce = int (*)(int, int*, int*, void**);
        using GetInstance = void* (*)();
        using Callback = void (*)(int64_t, void*);
        using Post = void (*)(void*, Callback, void*);

        static inline Post post = nullptr;
        static inline std::atomic<void*> choreographer{nullptr};
        // Nanoseconds on the clock that does not stop or jump. No period, nothing known yet.
        static inline std::atomic<int64_t> at{0};
        static inline std::atomic<int64_t> period{0};
        static inline std::atomic<int64_t> asked{0};
        static inline std::atomic<bool> asking{false};

        // The thread that listens lives for as long as the process does: a looper cannot be
        // asked to stop from outside.
        static bool Start() {
            const auto android = Library::Open("libandroid.so");
            if (!android) {
                return false;
            }
            const auto prepare = android->Func<Prepare>("ALooper_prepare");
            const auto pollOnce = android->Func<PollOnce>("ALooper_pollOnce");
            const auto instance = android->Func<GetInstance>("AChoreographer_getInstance");
            post = android->Func<Post>("AChoreographer_postFrameCallback64");
            // The older call counts the time in a `long`, which is as wide only on a 64-bit system.
            if (!post && sizeof(long) == sizeof(int64_t)) {
                post = android->Func<Post>("AChoreographer_postFrameCallback");
            }
            if (!prepare || !pollOnce || !instance || !post) {
                return false;
            }
            std::thread([=] {
                pthread_setname_np(pthread_self(), "vsync");
                prepare(0);
                choreographer.store(instance());
                for (;;) {
                    pollOnce(-1, nullptr, nullptr, nullptr);
                }
            }).detach();
            return true;
        }

        // Called by the choreographer on its thread at a refresh, with how many more refreshes
        // to be called at. The shortest time between two of the run is the period.
        static void OnRefresh(int64_t nanos, void* left) {
            static int64_t before = 0, shortest = 0;
            const auto more = reinterpret_cast<intptr_t>(left);
            shortest = more == 2 ? std::numeric_limits<int64_t>::max() : std::min(shortest, nanos - before);
            before = nanos;
            if (more > 0) {
                post(choreographer.load(), OnRefresh, reinterpret_cast<void*>(more - 1));
                return;
            }
            // No display takes its frames less than two milliseconds or more than a tenth of a second apart.
            if (shortest >= 2'000'000 && shortest <= 100'000'000) {
                period.store(shortest);
                at.store(nanos);
            }
            asking.store(false);
        }

        static std::optional<Vsync> Get() {
            static const bool Started = Start();
            void* const instance = choreographer.load();
            if (!Started || !instance) {
                return std::nullopt;
            }
            const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now - asked.load() > 250'000'000 && !asking.exchange(true)) {
                asked.store(now);
                post(instance, OnRefresh, reinterpret_cast<void*>(intptr_t{2}));
            }
            const int64_t known = period.load();
            if (known == 0) {
                return std::nullopt;
            }
            return Vsync{.at = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(at.load())), .period = std::chrono::nanoseconds(known)};
        }
    };

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
        made.control = TRY(gui->CreateSurface(client, name.c_str(), display.width, display.height, capture, LayerKind::Drawn));
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
        // The new layer starts without the blur the old one had.
        if (const auto blurred = PlaceBlur(std::nullopt); !blurred) {
            logger.Warn("{}", blurred.error().message);
        }
        return {};
    }

    // Asks the system for the blur under the parts kept in `blur`. The layer it is asked on is
    // made when there first is something to blur.
    Result<> PlaceBlur(std::optional<std::chrono::steady_clock::time_point> shownAt) {
        if (!layer.blur.Get()) {
            if (blur.empty()) {
                return {};
            }
            layer.blur = TRY(gui->CreateBlurLayer(client, transaction, layer.control, capture));
        }
        return gui->SetBlur(transaction, layer.blur.Get(), blur, shownAt);
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

std::optional<Vsync> NativeWindow::GetVsync() const {
    if (const auto vsync = State::Rhythm::Get()) {
        return vsync;
    }
    // An old system says how often its display takes a frame and not when: any moment will do.
    return state_->reportedPeriod.transform([](std::chrono::nanoseconds period) { return Vsync{.at = {}, .period = period}; });
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

bool NativeWindow::CanBlur() const {
    // The compositor says in this property whether it was built to blur.
    return state_->gui->CanBlur() && System::GetProperty("ro.surface_flinger.supports_background_blur") == "1";
}

Result<> NativeWindow::SetBlur(std::span<const BlurRegion> regions) {
    State& state = *state_;
    if (std::ranges::equal(regions, state.blur)) {
        // The frame drawn for the last change has gone: those after it are shown as soon as they can be.
        state.Stamp(std::nullopt);
        return {};
    }
    state.blur.assign(regions.begin(), regions.end());
    // The system takes a change like this at once, and a frame only after its turn in the
    // window's queue, two of the display's frames later: a blur that moves with what is drawn
    // would run ahead of it. So the change and the frame drawn for it are both told to be shown
    // at one moment that far ahead, which keeps the frame no longer than the queue does anyway.
    const auto shownAt = std::chrono::steady_clock::now() + 2 * GetVsync().transform([](const Vsync& vsync) { return vsync.period; }).value_or(std::chrono::nanoseconds(16'666'667));
    state.Stamp(shownAt);
    return state.PlaceBlur(shownAt);
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
