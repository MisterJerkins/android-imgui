#pragma once

#include "Window/ANativeWindowCreator.h"

#include "imgui.h"

#include <array>
#include <cstddef>
#include <span>
#include <vector>

struct ImGuiContext;

// The screen behind the interface, blurred: behind every window, behind any rectangle, or both.
// `Graphics::GetBlur` gives the one that belongs to the graphics.
//
// The system does the blurring (Android 12 and later, on devices whose compositor is built for
// it). Nothing is drawn for it here, and what is behind is blurred anew whenever it changes
// with the interface standing still. It costs the system the same for any radius, any size and
// any number of parts, more for every other radius in use at once, which is why a frame has
// three radii at the most, and nothing while the screen behind stays as it is. Where the
// system cannot blur, everything here does nothing.
//
// The blurred screen lies under all that the interface draws: it shows where that is not opaque.
class Blur {
public:
    explicit Blur(bool available);

    // Whether the system blurs at all. Nothing here needs this asked first.
    bool IsAvailable() const;

    float GetWindowRadius() const;
    // Blurs the screen behind every window by this many pixels, from the frame drawn next.
    // Zero, which is how it starts, for none. A window without a background is left out.
    void SetWindowRadius(float radius);

    // Blurs the screen behind a rectangle by `radius` pixels in the frame being built, so it is
    // called in every frame the blur is to stay. The place, the rounding and the clipping are
    // those of `list->AddRectFilled(min, max, colour, rounding)`.
    void AddRect(const ImDrawList* list, ImVec2 min, ImVec2 max, float radius, float rounding = 0.0f);

    // For the graphics, at the start of a frame: the rectangles of the frame before are forgotten.
    void NewFrame();
    // For the graphics, once Dear ImGui has made a frame up: the parts of the layer to blur in
    // it. Windows, and what is drawn into them, are `lift` pixels above where Dear ImGui has them.
    std::span<const BlurRegion> Collect(const ImGuiContext& context, float lift);

private:
    // What is to be blurred, in the pixels Dear ImGui counts in.
    struct Part {
        ImVec2 min;
        ImVec2 max;
        float radius = 0.0f;
        // Top left, top right, bottom left, bottom right.
        std::array<float, 4> corners{};
        // Whether it goes up with the windows when they make room for an on-screen keyboard.
        bool lifted = false;
    };

    void Push(const Part& part, float lift, ImVec2 screen);
    // How many different radii the parts collected so far have.
    size_t Radii() const;

    bool available_ = false;
    float windowRadius_ = 0.0f;
    std::vector<Part> parts_;
    std::vector<BlurRegion> regions_;
};
