#include "Graphics/Graphics.h"

#include "Graphics/OpenGL/OpenGLGraphics.h"
#include "Graphics/Software/SoftwareGraphics.h"
#include "Graphics/Vulkan/VulkanGraphics.h"
#include "Utils/Logger.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <utility>

using namespace std::chrono_literals;

// How often a frame is due at the least while nothing asks for one: the display is looked at.
static constexpr auto IdlePoll = 250ms;
// How long frames go on after the last input: what Dear ImGui shows with a delay, such as a
// tooltip, still appears. Frames that draw the same picture are not sent to the screen.
static constexpr auto SettleTime = 500ms;

Graphics::Graphics(const NativeWindow& window) : window_(window), generation_(window.GetGeneration()), blur_(window.CanBlur()) {
    const DisplayInfo display = window_.GetDisplayInfo();

    IMGUI_CHECKVERSION();
    context_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
    // A drag that starts on a window's content scrolls it, as on any touch screen; the title
    // bar is what moves a window.
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.DisplaySize = ImVec2(static_cast<float>(display.width), static_cast<float>(display.height));
    io.BackendPlatformName = "android-imgui";
}

// What draws has let go of its side by now; the textures and the context go last.
Graphics::~Graphics() {
    Texture::Collect();
    ImGui::DestroyContext(context_);
}

void Graphics::Ready() {
    const DisplayInfo display = window_.GetDisplayInfo();
    logger.Info("{}x{} at {} degrees, Android API {}, {}", display.width, display.height, display.GetDegrees(), window_.GetApiLevel(), GetName());

    LoadSystemFont();
    ImGui::StyleColorsDark();
    baseStyle_ = ImGui::GetStyle();
    baseStyle_.FontSizeBase = 14.0f;
    ApplyScale();

    const Clock::time_point now = Clock::now();
    started_ = now;
    polled_ = now;
    frameAt_ = now - IdlePoll;
    drawnAt_ = now;
    settleUntil_ = now + SettleTime;
    repaint_ = now;
}

void Graphics::LoadSystemFont() {
    ImGuiIO& io = ImGui::GetIO();
    for (const char* path : {"/system/fonts/RobotoStatic-Regular.ttf", "/system/fonts/Roboto-Regular.ttf", "/system/fonts/NotoSans-Regular.ttf", "/system/fonts/DroidSans.ttf"}) {
        if (access(path, R_OK) == 0 && io.Fonts->AddFontFromFileTTF(path)) {
            logger.Debug("font {}", path);
            return;
        }
    }
    logger.Warn("no system font was found: text is limited to Latin letters");
    io.Fonts->AddFontDefault();
}

uint64_t Graphics::Fingerprint(const ImDrawData& frame) {
    uint64_t hash = Utils::HashSeed ^ (static_cast<uint64_t>(frame.DisplaySize.x) << 32 | static_cast<uint64_t>(frame.DisplaySize.y));
    const auto mix = [&hash](const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (; size >= 8; bytes += 8, size -= 8) {
            uint64_t word = 0;
            std::memcpy(&word, bytes, 8);
            hash = Utils::HashMix(hash, word);
        }
        for (; size > 0; bytes++, size--) {
            hash = Utils::HashMix(hash, *bytes);
        }
    };
    for (const ImDrawList* list : frame.CmdLists) {
        for (const ImDrawCmd& command : list->CmdBuffer) {
            const uint64_t words[] = {reinterpret_cast<uintptr_t>(command.TexRef._TexData), command.TexRef._TexID, command.ElemCount, command.VtxOffset, command.IdxOffset};
            mix(&command.ClipRect, sizeof command.ClipRect);
            mix(words, sizeof words);
        }
        mix(list->IdxBuffer.Data, static_cast<size_t>(list->IdxBuffer.Size) * sizeof(ImDrawIdx));
        mix(list->VtxBuffer.Data, static_cast<size_t>(list->VtxBuffer.Size) * sizeof(ImDrawVert));
    }
    return hash;
}

bool Graphics::Fail(const Error& error) {
    error_ = error.message;
    logger.Error("{}", error_);
    return false;
}

void Graphics::ApplyScale() {
    const float density = window_.GetDisplayInfo().density;
    ImGuiStyle& style = ImGui::GetStyle();
    ImGuiStyle next = baseStyle_;
    std::ranges::copy(style.Colors, next.Colors);
    next.ScaleAllSizes(density * scale_);
    next.FontScaleDpi = density;
    next.FontScaleMain = scale_;
    style = next;
    // A finger is less steady than a mouse: what tells a drag from a tap grows with the screen.
    ImGui::GetIO().MouseDragThreshold = 6.0f * density;
}

void Graphics::Want(Clock::time_point at) {
    if (!repaintWanted_ || at < *repaintWanted_) {
        repaintWanted_ = at;
    }
}

// Sleeps until a frame is due: one for each of the display's at the most, and none while
// nothing asks for one.
Result<> Graphics::Wait(InputSource* input) {
    // A frame begins once the display has taken its turn after the frame before began. Begun
    // sooner it would wait in the window's queue, and hold back every frame after it for as
    // long. Without the display's rhythm to go by, no display is faster than a frame in four
    // milliseconds. Only frames that come one after another are held to that rhythm, so only
    // then is the system asked for it.
    Clock::time_point earliest = frameAt_ + 4ms;
    if (const std::optional<Vsync> vsync = Clock::now() - frameAt_ < 100ms ? window_.GetVsync() : std::nullopt) {
        const Clock::duration since = frameAt_ - vsync->at;
        earliest = vsync->at + (since < Clock::duration::zero() ? -(-since / vsync->period) : since / vsync->period + 1) * vsync->period;
    }
    bool pending = false;
    for (;;) {
        const Clock::time_point now = Clock::now();
        // The display is asked about ten times a second: that is how fast a rotation is followed.
        if (now - polled_ >= 100ms) {
            polled_ = now;
            if (input) {
                input->Rest();
            }
            pending |= TRY(window_.FollowDisplay());
        }
        if (window_.GetGeneration() != generation_) {
            // The window was made again or resized: the drawing moves to it, and the layer it
            // has left may go.
            TRY(Rebind(window_));
            window_.ReleaseOld();
            generation_ = window_.GetGeneration();
            print_ = 0;
            repaint_ = Clock::now();
        }
        const std::optional<Clock::time_point> due = pending ? std::optional(earliest) : repaint_.transform([&](Clock::time_point at) { return std::max(at, earliest); });
        const Clock::duration pause = std::min<Clock::duration>(due ? *due - now : Clock::duration(IdlePoll), IdlePoll);
        if (pause <= Clock::duration::zero()) {
            break;
        }
        if (pending || !input) {
            std::this_thread::sleep_for(pause);
        } else {
            pending = input->Wait(pause);
        }
    }
    frameAt_ = Clock::now();
    return {};
}

Result<bool> Graphics::Begin() {
    const std::shared_ptr<InputSource> input = input_.lock();
    TRY(Wait(input.get()));
    if (!running_) {
        return false;
    }

    const DisplayInfo display = window_.GetDisplayInfo();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(display.width), static_cast<float>(display.height));
    io.DeltaTime = std::max(std::chrono::duration<float>(frameAt_ - drawnAt_).count(), 1e-4f);
    drawnAt_ = frameAt_;
    repaintAsked_ = false;
    repaintWanted_.reset();
    if (input) {
        // Seconds since the graphics started, which is how the input tells time.
        const double time = std::chrono::duration<double>(frameAt_ - started_).count();
        const InputSource::Wish wish = input->TakeInput(time, display, scale_);
        if (wish.settle) {
            settleUntil_ = Clock::now() + SettleTime;
        }
        if (wish.due) {
            Want(frameAt_ + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(std::max(*wish.due - time, 0.0))));
        }
    }
    blur_.NewFrame();
    ImGui::NewFrame();
    if (input && input->AfterNewFrame()) {
        repaintAsked_ = true;
    }
    return true;
}

Result<> Graphics::Finish() {
    const std::shared_ptr<InputSource> input = input_.lock();
    if (input) {
        input->BeforeRender();
    }
    ImGui::Render();
    // Before the frame goes to the window, which then has the two shown together.
    const std::span<const BlurRegion> blurred = blur_.Collect(*context_, input ? input->GetLift() : 0.0f);
    if (const auto placed = window_.SetBlur(blurred); !placed) {
        // The interface is all there without it: the reason is told and the blur given up.
        logger.Warn("{}", placed.error().message);
        blur_ = Blur(false);
    }
    ImDrawData& frame = *ImGui::GetDrawData();
    const uint64_t fresh = Fingerprint(frame);
    // Textures to create, update or destroy are work for the drawing even when the picture is the same.
    const bool textures = frame.Textures && std::ranges::any_of(*frame.Textures, [](const ImTextureData* texture) { return texture->Status != ImTextureStatus_OK; });
    if (fresh != print_ || textures) {
        TRY(Render(window_, frame));
        print_ = fresh;
    }
    const bool collecting = Texture::Collect();
    const bool typing = input && input->AfterRender(window_.GetDisplayInfo());

    // A frame with nothing in it leaves nothing of the layer on the screen.
    TRY(window_.SetVisible(frame.TotalVtxCount > 0 || !blurred.empty()));

    const ImGuiIO& io = ImGui::GetIO();
    const Clock::time_point now = Clock::now();
    const bool held = std::ranges::any_of(io.MouseDown, std::identity{});
    // Dear ImGui takes a tap in over several frames, one each for the move, the press and the
    // release. What it still holds back is owed a frame, however long this one has taken.
    const bool queued = context_->InputEventsQueue.Size > 0;
    if (repaintAsked_ || collecting || typing || held || queued || now < settleUntil_) {
        repaint_ = now;
    } else if (io.WantTextInput) {
        // A text cursor blinks; this is how often it is looked at.
        repaint_ = now + 200ms;
    } else {
        repaint_.reset();
    }
    if (repaintWanted_ && (!repaint_ || *repaintWanted_ < *repaint_)) {
        repaint_ = repaintWanted_;
    }
    return {};
}

bool Graphics::NewFrame() {
    const Result<bool> started = Begin();
    return started ? *started : Fail(started.error());
}

bool Graphics::EndFrame() {
    const Result<> drawn = Finish();
    if (!drawn) {
        // What went wrong ends the frames: the next `NewFrame` says so.
        running_ = false;
        return Fail(drawn.error());
    }
    return true;
}

void Graphics::Close() {
    running_ = false;
}

void Graphics::RequestRepaint() {
    repaintAsked_ = true;
}

void Graphics::RequestRepaintAfter(std::chrono::milliseconds delay) {
    Want(Clock::now() + delay);
}

const std::string& Graphics::GetError() const {
    return error_;
}

NativeWindow& Graphics::GetWindow() {
    return window_;
}

float Graphics::GetScale() const {
    return scale_;
}

Blur& Graphics::GetBlur() {
    return blur_;
}

void Graphics::SetScale(float scale) {
    scale = std::clamp(scale, 0.25f, 8.0f);
    if (scale == scale_) {
        return;
    }
    scale_ = scale;
    ApplyScale();
    repaintAsked_ = true;
}

ImGuiStyle& Graphics::GetBaseStyle() {
    return baseStyle_;
}

Texture Graphics::LoadImage(std::string_view name, std::span<const uint8_t> data) {
    const auto picture = Texture::Decode(name, data);
    if (!picture) {
        Fail(picture.error());
        return {};
    }
    return LoadPixels(name, picture->size, picture->rgba);
}

Texture Graphics::LoadImageFile(const std::string& path) {
    const auto data = Utils::ReadFile(path);
    if (!data) {
        Fail(data.error());
        return {};
    }
    return LoadImage(path, std::span(reinterpret_cast<const uint8_t*>(data->data()), data->size()));
}

Texture Graphics::LoadPixels(std::string_view name, std::array<size_t, 2> size, std::span<const uint8_t> rgba) {
    auto texture = Texture::Load(name, size, rgba, GetMaxTextureSide());
    if (!texture) {
        Fail(texture.error());
        return {};
    }
    return std::move(*texture);
}

void Graphics::SetInput(std::weak_ptr<InputSource> input) {
    input_ = std::move(input);
}

std::string GraphicsManager::error_;

std::optional<GraphicsApi> GraphicsManager::ParseApi(std::string_view name) {
    const std::string lower = Utils::ToLower(name);
    if (lower == "auto") {
        return GraphicsApi::Auto;
    }
    if (lower == "software" || lower == "cpu") {
        return GraphicsApi::Software;
    }
    if (lower == "opengl" || lower == "gl" || lower == "gles") {
        return GraphicsApi::OpenGL;
    }
    if (lower == "vulkan" || lower == "vk") {
        return GraphicsApi::Vulkan;
    }
    return std::nullopt;
}

std::unique_ptr<Graphics> GraphicsManager::Create(const NativeWindow& window, GraphicsApi api) {
    const auto fail = [](const Error& error) {
        error_ = error.message;
        logger.Error("{}", error_);
        return nullptr;
    };
    if (!window) {
        return fail(Error{"there is no window to draw into"});
    }
    const auto create = [](GraphicsApi candidate, const NativeWindow& layer) -> Result<std::unique_ptr<Graphics>> {
        switch (candidate) {
            case GraphicsApi::Vulkan:
                return VulkanGraphics::Create(layer);
            case GraphicsApi::Software:
                return SoftwareGraphics::Create(layer);
            default:
                return OpenGLGraphics::Create(layer);
        }
    };
    // The ways of drawing to try, best first.
    static constexpr GraphicsApi Any[] = {GraphicsApi::OpenGL, GraphicsApi::Vulkan, GraphicsApi::Software};
    const std::span<const GraphicsApi> candidates = api == GraphicsApi::Auto ? std::span<const GraphicsApi>(Any) : std::span<const GraphicsApi>(&api, 1);
    NativeWindow layer = window;
    Error failure{"no graphics backend to try"};
    for (const GraphicsApi candidate : candidates) {
        // Each attempt after the first gets a layer of its own: a window that one graphics API
        // has claimed is not given up to another.
        if (&candidate != candidates.data()) {
            if (const Result<> made = layer.Recreate(); !made) {
                return fail(made.error());
            }
            layer.ReleaseOld();
        }
        auto made = create(candidate, layer);
        if (made) {
            return std::move(*made);
        }
        logger.Warn("a backend is unavailable: {}", made.error().message);
        failure = std::move(made.error());
    }
    return fail(failure);
}

const std::string& GraphicsManager::GetError() {
    return error_;
}
