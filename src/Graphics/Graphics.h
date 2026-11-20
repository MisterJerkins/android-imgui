#pragma once

#include "Graphics/Blur.h"
#include "Graphics/Texture.h"
#include "Utils/Utils.h"
#include "Window/ANativeWindowCreator.h"

#include "imgui.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

enum class GraphicsApi { Auto, OpenGL, Vulkan, Software };

// What a frame is given from outside: the touches and keys that came, and someone to tell where
// the interface is. `touch.Init` puts one in; without it frames come on request only.
class InputSource {
public:
    // What the input asks of the frames to come.
    struct Wish {
        // Input came, so what Dear ImGui shows with a delay is still owed its frames.
        bool settle = false;
        // A frame at this time at the latest, on the clock `TakeInput` is given: a key held
        // down types again.
        std::optional<double> due;
    };

    virtual ~InputSource() = default;

    // Sleeps until input has come or the time is up. `true` when it has.
    virtual bool Wait(std::chrono::nanoseconds timeout) = 0;
    // Called now and then between frames.
    virtual void Rest() = 0;

    // Before `ImGui::NewFrame`: hands Dear ImGui the input that came. `time` is in seconds
    // since the graphics started, `scale` the user's own scale of the interface.
    virtual Wish TakeInput(double time, const DisplayInfo& display, float scale) = 0;
    // Right after `ImGui::NewFrame`. `true` to have another frame after this one.
    virtual bool AfterNewFrame() = 0;
    // Right before `ImGui::Render`: draws what the input shows itself.
    virtual void BeforeRender() = 0;
    // After `ImGui::Render`: how far up the frame's windows are drawn from where Dear ImGui
    // has them.
    virtual float GetLift() const = 0;
    // After the frame is drawn: tells where the interface is. `true` to have another frame.
    virtual bool AfterRender(const DisplayInfo& display) = 0;
};

// Dear ImGui drawing into the window. This class is what every way of drawing shares: the
// context, the frames and when they come, the scale, the pictures. `OpenGLGraphics`,
// `VulkanGraphics` and `SoftwareGraphics` are the ways of drawing; `GraphicsManager::Create`
// gives the one that was asked for.
//
// A way of drawing keeps the textures ImGui asks for (`ImGuiBackendFlags_RendererHasTextures`),
// so fonts of any size and the application's own pictures go the same way, and it takes vertex
// offsets (`ImGuiBackendFlags_RendererHasVtxOffset`).
//
// What can fail returns `false`; the reason is in the log and in `GetError`. The graphics are
// to be let go of before the program ends, not while it does: at the very end of a process the
// system's own graphics libraries are taken down first.
class Graphics {
public:
    Graphics(const Graphics&) = delete;
    Graphics& operator=(const Graphics&) = delete;
    virtual ~Graphics();

    // Starts a frame, to be built with Dear ImGui calls and finished with `EndFrame`. Sleeps
    // until there is something to draw: input, or a repaint that was asked for. `false` once
    // `close` was called, or when something failed.
    bool NewFrame();
    // Draws the frame, unless it shows the same picture as the last one.
    bool EndFrame();

    // Makes the next `NewFrame` return `false`.
    void Close();

    // Asks for another frame right after this one: for things that move on their own.
    void RequestRepaint();
    // Asks for a frame once `delay` has passed, if none comes sooner.
    void RequestRepaintAfter(std::chrono::milliseconds delay);

    // The name of the way of drawing.
    virtual const char* GetName() const = 0;
    // What went wrong when something last returned `false`.
    const std::string& GetError() const;
    // The window the frames go to: the same layer as the handle the graphics were made on.
    NativeWindow& GetWindow();

    float GetScale() const;
    // Makes the whole interface larger or smaller, on top of the display's own density.
    void SetScale(float scale);
    // The screen behind the interface, blurred: behind its windows, or behind any rectangle.
    Blur& GetBlur();
    // The style before any scaling. Change sizes here, then call `SetScale`; colours can be
    // changed in `ImGui::GetStyle()` directly.
    ImGuiStyle& GetBaseStyle();

    // Loads a PNG or JPEG picture. `name` tells the picture from others in messages. An empty
    // texture when it cannot be loaded.
    Texture LoadImage(std::string_view name, std::span<const uint8_t> data);
    // Loads a PNG or JPEG picture from a file.
    Texture LoadImageFile(const std::string& path);
    // Loads a picture from RGBA pixels, four bytes each, row after row from the top.
    Texture LoadPixels(std::string_view name, std::array<size_t, 2> size, std::span<const uint8_t> rgba);

    // Where the frames take their input from; `touch.Init` calls this.
    void SetInput(std::weak_ptr<InputSource> input);

protected:
    // Makes the Dear ImGui context, which a way of drawing needs before it starts.
    explicit Graphics(const NativeWindow& window);
    // Called by a way of drawing once it can draw: the font, the style, the first frame's time.
    void Ready();

    // The longest side a texture may have.
    virtual size_t GetMaxTextureSide() const = 0;
    // Moves to the window as it is now: the layer was made again, or resized.
    virtual Result<> Rebind(const NativeWindow& window) = 0;
    // Draws one frame, creating, updating and destroying the textures it asks for on the way.
    virtual Result<> Render(const NativeWindow& window, ImDrawData& frame) = 0;

private:
    using Clock = std::chrono::steady_clock;

    // The system's own font has the letters of most alphabets; Dear ImGui's built-in one only Latin.
    static void LoadSystemFont();
    // Tells one frame's picture from another's, so a frame that changes nothing is not drawn.
    static uint64_t Fingerprint(const ImDrawData& frame);

    // Keeps the reason of a failure, tells the log, and is the `false` to return.
    bool Fail(const Error& error);
    void ApplyScale();
    void Want(Clock::time_point at);
    Result<> Wait(InputSource* input);
    Result<bool> Begin();
    Result<> Finish();
    NativeWindow window_;
    ImGuiContext* context_ = nullptr;
    std::weak_ptr<InputSource> input_;
    // The generation of the window the way of drawing is bound to.
    uint64_t generation_ = 0;
    ImGuiStyle baseStyle_;
    float scale_ = 1.0f;
    Blur blur_;
    Clock::time_point started_ = Clock::now();
    Clock::time_point polled_;
    Clock::time_point frameAt_;
    Clock::time_point drawnAt_;
    Clock::time_point settleUntil_;
    std::optional<Clock::time_point> repaint_;
    std::optional<Clock::time_point> repaintWanted_;
    uint64_t print_ = 0;
    bool running_ = true;
    bool repaintAsked_ = false;
    std::string error_;
};

// Picks the way of drawing, as `ANativeWindowCreator` makes the window.
class GraphicsManager {
public:
    // `auto`, `gl` or `gles` or `opengl`, `vk` or `vulkan`, `cpu` or `software`, in any case.
    static std::optional<GraphicsApi> ParseApi(std::string_view name);
    // Brings Dear ImGui up on the window with the graphics asked for. `GraphicsApi::Auto` takes
    // the first that works: OpenGL ES, then Vulkan, then the processor. Nothing when none
    // starts; `GetError` says why.
    static std::unique_ptr<Graphics> Create(const NativeWindow& window, GraphicsApi api = GraphicsApi::Auto);
    // What went wrong when `Create` last gave nothing.
    static const std::string& GetError();

private:
    static std::string error_;
};
