#include "Graphics/Blur.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

Blur::Blur(bool available) : available_(available) {
}

bool Blur::IsAvailable() const {
    return available_;
}

float Blur::GetWindowRadius() const {
    return windowRadius_;
}

void Blur::SetWindowRadius(float radius) {
    windowRadius_ = std::max(radius, 0.0f);
}

void Blur::AddRect(const ImDrawList* list, ImVec2 min, ImVec2 max, float radius, float rounding) {
    if (!available_) {
        return;
    }
    Part part{.min = min, .max = max, .radius = radius};
    // Cut the way the list cuts what is drawn into it now. A corner keeps its rounding only
    // while neither of its sides was cut.
    if (list->_ClipRectStack.Size > 0) {
        const ImVec4& clip = list->_ClipRectStack.back();
        part.min = ImMax(min, ImVec2(clip.x, clip.y));
        part.max = ImMin(max, ImVec2(clip.z, clip.w));
    }
    const bool left = part.min.x == min.x, top = part.min.y == min.y, right = part.max.x == max.x, bottom = part.max.y == max.y;
    part.corners = {left && top ? rounding : 0.0f, right && top ? rounding : 0.0f, left && bottom ? rounding : 0.0f, right && bottom ? rounding : 0.0f};
    // What is drawn behind and in front of all the windows stays where it is when they go up
    // above an on-screen keyboard; everything else goes with them.
    const ImGuiViewportP& viewport = *ImGui::GetCurrentContext()->Viewports[0];
    part.lifted = list != viewport.BgFgDrawLists[0] && list != viewport.BgFgDrawLists[1];
    parts_.push_back(part);
}

void Blur::NewFrame() {
    parts_.clear();
}

std::span<const BlurRegion> Blur::Collect(const ImGuiContext& context, float lift) {
    regions_.clear();
    if (!available_) {
        return regions_;
    }
    const ImVec2 screen = context.IO.DisplaySize;
    if (windowRadius_ > 0.0f) {
        for (const ImGuiWindow* window : context.Windows) {
            // A child is drawn inside its parent, which is there already, and a window without
            // a background has nothing of its own to show the blur through.
            if (!window->Active || window->Hidden || (window->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_NoBackground)) != 0) {
                continue;
            }
            const float corner = window->WindowRounding;
            Push({.min = window->Pos, .max = {window->Pos.x + window->Size.x, window->Pos.y + window->Size.y}, .radius = windowRadius_, .corners = {corner, corner, corner, corner}, .lifted = true}, lift, screen);
        }
    }
    for (const Part& part : parts_) {
        Push(part, lift, screen);
    }
    return regions_;
}

void Blur::Push(const Part& part, float lift, ImVec2 screen) {
    const float up = part.lifted ? lift : 0.0f;
    BlurRegion region{
        .radius = static_cast<uint32_t>(std::lround(std::max(part.radius, 0.0f))),
        .cornerTopLeft = part.corners[0],
        .cornerTopRight = part.corners[1],
        .cornerBottomLeft = part.corners[2],
        .cornerBottomRight = part.corners[3],
        .alpha = 1.0f,
        .left = static_cast<int32_t>(std::lround(std::clamp(part.min.x, 0.0f, screen.x))),
        .top = static_cast<int32_t>(std::lround(std::clamp(part.min.y - up, 0.0f, screen.y))),
        .right = static_cast<int32_t>(std::lround(std::clamp(part.max.x, 0.0f, screen.x))),
        .bottom = static_cast<int32_t>(std::lround(std::clamp(part.max.y - up, 0.0f, screen.y))),
    };
    // Nothing to see of it: no radius, or no part of it on the screen. Past sixty-four parts,
    // which is as many as the system was seen to take in its stride, the rest go without.
    if (region.radius == 0 || region.right <= region.left || region.bottom <= region.top || regions_.size() == 64) {
        return;
    }
    // The system goes over the screen once more for every radius in use, and that is what
    // its blurring costs. So a frame has three radii at the most: a fourth takes the nearest.
    const auto same = std::ranges::find(regions_, region.radius, &BlurRegion::radius);
    if (same == regions_.end() && Radii() == 3) {
        region.radius = std::ranges::min_element(regions_, {}, [&](const BlurRegion& other) { return std::abs(static_cast<int64_t>(other.radius) - static_cast<int64_t>(region.radius)); })->radius;
    }
    regions_.push_back(region);
}

size_t Blur::Radii() const {
    size_t count = 0;
    for (auto region = regions_.begin(); region != regions_.end(); ++region) {
        count += std::ranges::find(regions_.begin(), region, region->radius, &BlurRegion::radius) == region ? 1 : 0;
    }
    return count;
}
