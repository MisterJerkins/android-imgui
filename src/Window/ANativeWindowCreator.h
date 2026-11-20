#pragma once

#include "Utils/Utils.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct ANativeWindow;

enum class Rotation : uint8_t { Deg0, Deg90, Deg180, Deg270 };

// Whether screenshots and screen recordings see the layer.
enum class Capture : uint8_t { Visible, Hidden, Secure };

struct DisplayInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    Rotation rotation = Rotation::Deg0;
    float density = 1.0f;

    bool operator==(const DisplayInfo&) const = default;

    constexpr uint32_t GetDegrees() const {
        return static_cast<uint32_t>(rotation) * 90;
    }
};

// A rectangle of pixels, as `ARect` lays it out.
struct Region {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;

    bool operator==(const Region&) const = default;

    constexpr bool IsEmpty() const {
        return right <= left || bottom <= top;
    }

    constexpr int64_t GetArea() const {
        return IsEmpty() ? 0 : static_cast<int64_t>(right - left) * static_cast<int64_t>(bottom - top);
    }

    constexpr Region Intersect(Region other) const {
        return {std::max(left, other.left), std::max(top, other.top), std::min(right, other.right), std::min(bottom, other.bottom)};
    }

    // The smallest region holding both; an empty one adds nothing.
    constexpr Region Unite(Region other) const {
        if (IsEmpty()) {
            return other;
        }
        if (other.IsEmpty()) {
            return *this;
        }
        return {std::min(left, other.left), std::min(top, other.top), std::max(right, other.right), std::max(bottom, other.bottom)};
    }
};

// One buffer of the window, locked for drawing on the processor. It is shown when the buffer goes away.
class WindowBuffer {
public:
    WindowBuffer(WindowBuffer&& other) noexcept;
    WindowBuffer& operator=(WindowBuffer&&) = delete;
    ~WindowBuffer();

    size_t GetWidth() const {
        return width_;
    }

    size_t GetHeight() const {
        return height_;
    }

    // One row of premultiplied RGBA pixels, four bytes each.
    std::span<uint32_t> GetRow(size_t y);
    // Every row, a stride of pixels apart.
    std::span<uint32_t> GetPixels();

private:
    friend class NativeWindow;
    WindowBuffer(ANativeWindow* window, uint32_t* pixels, size_t width, size_t height, size_t stride);

    ANativeWindow* window_;
    uint32_t* pixels_;
    size_t width_;
    size_t height_;
    size_t stride_;
};

// The display's own rhythm: the moment it took a frame, and the time between two of its frames.
struct Vsync {
    std::chrono::steady_clock::time_point at;
    std::chrono::nanoseconds period{};
};

// A part of the layer under which the system blurs the screen, in the layer's own pixels and
// laid out the way the system's library takes it. Whatever the layer draws there goes on top
// of the blurred picture, so it shows where the layer is not opaque.
struct BlurRegion {
    uint32_t radius = 0;
    float cornerTopLeft = 0.0f;
    float cornerTopRight = 0.0f;
    float cornerBottomLeft = 0.0f;
    float cornerBottomRight = 0.0f;
    float alpha = 1.0f;
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;

    bool operator==(const BlurRegion&) const = default;
};

// The layer over the whole screen, above everything else, and its window: what EGL, Vulkan and
// the processor draw into. Copies are handles to the one layer, which goes when the last of
// them does.
class NativeWindow {
public:
    // No window: what `ANativeWindowCreator::Create` returns when it fails.
    NativeWindow() = default;

    explicit operator bool() const {
        return static_cast<bool>(state_);
    }

    ANativeWindow* Get() const;
    // Counts up every time the window is replaced or resized: whatever draws into it has to be
    // bound to it again.
    uint64_t GetGeneration() const;

    // The display as it was when last looked at: `FollowDisplay` looks.
    DisplayInfo GetDisplayInfo() const;
    // The display's unique id as Android's input configuration names it.
    std::optional<std::string> GetDisplayId() const;
    uint32_t GetApiLevel() const;
    // When the display takes its frames, where the system tells.
    std::optional<Vsync> GetVsync() const;
    // Which symbols of the system library are in use, one line each.
    std::vector<std::string> GetSymbols() const;

    Capture GetCapture() const;
    // `false` when the layer could not be made anew for it; `GetError` says why.
    bool SetCapture(Capture capture);
    // What went wrong when something here last returned `false`.
    const std::string& GetError() const;

    bool IsMirroring() const;
    // From Android 14 a screen recording misses the layer unless a copy of it is put on the
    // recorder's display. That is off to begin with, since keeping the copies up means asking
    // the system for its displays every second. Nothing is copied while `Capture` hides the
    // layer, and before Android 14 recordings see it without help.
    void SetMirroring(bool mirroring);

    Result<> SetVisible(bool visible);

    // Whether the system blurs what is under a layer: Android 12 and later, on devices whose
    // compositor is built for it.
    bool CanBlur() const;
    // Blurs the screen under the given parts of the layer; none of them, no blur. The system
    // does the blurring itself, every time what is underneath changes, with nothing drawn here.
    // To be called before the frame drawn for the change goes to the window: the two are then
    // shown in the same display frame. What fails is not tried again until the parts change.
    Result<> SetBlur(std::span<const BlurRegion> regions);

    // Takes up a rotated or resized display. `true` when it has changed.
    Result<bool> FollowDisplay();
    // Puts a new layer in place of this one: a window that one graphics API has claimed is not
    // given up to another. The old layer stays until `ReleaseOld`, so that whatever drew into it
    // can let go of it first.
    Result<> Recreate();
    void ReleaseOld();

    // Locks the next buffer for drawing on the processor.
    Result<WindowBuffer> Lock() const;

private:
    friend class ANativeWindowCreator;
    struct State;

    explicit NativeWindow(std::shared_ptr<State> state);

    std::shared_ptr<State> state_;
};

// Makes the layer from a plain root process, through SurfaceFlinger's own client library.
class ANativeWindowCreator {
public:
    // The main display as it is now; all zeros when it cannot be asked.
    static DisplayInfo GetDisplayInfo();
    // Puts a layer the size of the display over everything else. `name` is what the system's
    // own listings call it. An empty window when that fails; `GetError` says why.
    static NativeWindow Create(std::string_view name, Capture capture = Capture::Visible);
    // What went wrong when `Create` or `GetDisplayInfo` last failed.
    static const std::string& GetError();

private:
    static std::string error_;
};
