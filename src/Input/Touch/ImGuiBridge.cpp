#include "Input/Touch/ImGuiBridge.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

// The speed below which a glide stops.
constexpr float SlowestGlide = 30.0f;
// How much of a frame's movement goes into the speed a glide starts with.
constexpr float Smoothing = 0.5f;

int ImGuiBridge::ButtonIndex(MouseButton button) {
    switch (button) {
        case MouseButton::Left:
            return 0;
        case MouseButton::Right:
            return 1;
        case MouseButton::Middle:
            return 2;
    }
    return 0;
}

bool ImGuiBridge::CanScroll(const ImGuiWindow& window, int axis) {
    const bool locked = (window.Flags & ImGuiWindowFlags_NoScrollWithMouse) != 0;
    return !locked && (axis == 0 ? window.ScrollMax.x : window.ScrollMax.y) > 0.0f;
}

ImGuiWindow* ImGuiBridge::Scrollable(ImGuiWindow* window, int axis) {
    for (; window; window = (window->Flags & ImGuiWindowFlags_ChildWindow) ? window->ParentWindow : nullptr) {
        if (CanScroll(*window, axis)) {
            return window;
        }
    }
    return nullptr;
}

bool ImGuiBridge::FreeToScroll(const ImGuiContext& context, ImGuiWindow& window, ImVec2 landed) {
    if (context.ActiveId == 0) {
        return true;
    }
    ImGuiWindow* root = window.RootWindow;
    if (context.ActiveId == root->MoveId || context.ActiveId == window.MoveId) {
        return !root->TitleBarRect().Contains(landed);
    }
    if (context.ActiveIdUsingNavDirMask != 0) {
        return false;
    }
    for (ImGuiWindow* owner = context.ActiveIdWindow; owner; owner = (owner->Flags & ImGuiWindowFlags_ChildWindow) ? owner->ParentWindow : nullptr) {
        for (const ImGuiAxis axis : {ImGuiAxis_X, ImGuiAxis_Y}) {
            if (context.ActiveId == ImGui::GetWindowScrollbarID(owner, axis)) {
                return false;
            }
        }
        for (int corner = 0; corner < 4; corner++) {
            if (context.ActiveId == ImGui::GetWindowResizeCornerID(owner, corner)) {
                return false;
            }
        }
        for (int border = 0; border < 4; border++) {
            if (context.ActiveId == ImGui::GetWindowResizeBorderID(owner, static_cast<ImGuiDir>(border))) {
                return false;
            }
        }
    }
    return true;
}

bool ImGuiBridge::Feed(const InputEvent& event) {
    ImGuiIO& io = ImGui::GetIO();
    const auto source = [&] { io.AddMouseSourceEvent(touch_ ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse); };
    const Utils::Overloaded take{
        [&](const InputEvent::Touch& finger) {
            // The pointer events that follow say where the finger in charge is;
            // this only tells that it is a finger.
            if (finger.phase == Phase::Start) {
                fingers_++;
                touch_ = true;
            } else if (finger.phase != Phase::Move && fingers_ > 0) {
                fingers_--;
            }
            return true;
        },
        [&](const InputEvent::PointerMoved& moved) {
            source();
            io.AddMousePosEvent(moved.position[0], moved.position[1]);
            return true;
        },
        [&](const InputEvent::PointerButton& button) {
            source();
            io.AddMousePosEvent(button.position[0], button.position[1]);
            io.AddMouseButtonEvent(ButtonIndex(button.button), button.pressed);
            return true;
        },
        [&](const InputEvent::PointerGone&) {
            source();
            // A finger that left the interface without lifting off it lets go of
            // what it held from nowhere, so nothing is clicked.
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            if (touch_) {
                io.AddMouseButtonEvent(0, false);
            }
            touch_ = fingers_ > 0;
            return true;
        },
        [&](const InputEvent::Wheel& wheel) {
            io.AddMouseWheelEvent(-wheel.x, wheel.y);
            return true;
        },
        [](const InputEvent::Key&) { return false; },
        [](const InputEvent::KeyboardTouch&) { return false; },
    };
    return std::visit(take, event.value);
}

bool ImGuiBridge::Scroll() {
    ImGuiContext& context = *ImGui::GetCurrentContext();
    const ImGuiIO& io = context.IO;
    const bool finger = io.MouseSource == ImGuiMouseSource_TouchScreen;

    if (io.MouseClicked[0]) {
        // A new touch stops whatever was gliding.
        landed_ = finger && context.HoveredWindow ? context.HoveredWindow->ID : 0;
        if (landed_ != 0) {
            resting_ = context.HoveredWindow->RootWindow->Pos;
        }
        scrolled_ = 0;
        dragging_ = false;
        velocity_ = ImVec2(0.0f, 0.0f);
    }

    if (io.MouseDown[0] && finger && landed_ != 0) {
        const ImVec2 moved(io.MousePos.x - io.MouseClickedPos[0].x, io.MousePos.y - io.MouseClickedPos[0].y);
        if (!dragging_ && std::hypot(moved.x, moved.y) > io.MouseDragThreshold) {
            ImGuiWindow* under = ImGui::FindWindowByID(landed_);
            const int axis = std::fabs(moved.x) > std::fabs(moved.y) ? 0 : 1;
            ImGuiWindow* target = under ? Scrollable(under, axis) : nullptr;
            if (target && FreeToScroll(context, *under, io.MouseClickedPos[0])) {
                // Whatever the finger landed on lets go: the drag is a scroll now.
                if (context.MovingWindow) {
                    ImGui::SetWindowPos(context.MovingWindow->RootWindow, resting_);
                    context.MovingWindow = nullptr;
                }
                ImGui::SetActiveID(target->GetID("##touch-scroll"), target);
                scrolled_ = target->ID;
                dragging_ = true;
            } else {
                landed_ = 0;
            }
        }
        if (dragging_) {
            ImGuiWindow* target = ImGui::FindWindowByID(scrolled_);
            if (!target) {
                dragging_ = false;
                return false;
            }
            ImGui::KeepAliveID(target->GetID("##touch-scroll"));
            if (CanScroll(*target, 0)) {
                ImGui::SetScrollX(target, target->Scroll.x - io.MouseDelta.x);
            }
            if (CanScroll(*target, 1)) {
                ImGui::SetScrollY(target, target->Scroll.y - io.MouseDelta.y);
            }
            const float rate = 1.0f / std::max(io.DeltaTime, 1e-4f);
            velocity_.x += (io.MouseDelta.x * rate - velocity_.x) * Smoothing;
            velocity_.y += (io.MouseDelta.y * rate - velocity_.y) * Smoothing;
            return true;
        }
        return false;
    }

    if (dragging_) {
        dragging_ = false;
        if (ImGuiWindow* target = ImGui::FindWindowByID(scrolled_); target && context.ActiveId == target->GetID("##touch-scroll")) {
            ImGui::ClearActiveID();
        }
    }
    if (scrolled_ == 0) {
        return false;
    }

    ImGuiWindow* target = ImGui::FindWindowByID(scrolled_);
    const bool horizontal = target && CanScroll(*target, 0) && std::fabs(velocity_.x) > SlowestGlide;
    const bool vertical = target && CanScroll(*target, 1) && std::fabs(velocity_.y) > SlowestGlide;
    if (!horizontal && !vertical) {
        scrolled_ = 0;
        return false;
    }
    const float step = std::min(io.DeltaTime, 0.1f);
    const ImVec2 before = target->Scroll;
    if (horizontal) {
        ImGui::SetScrollX(target, std::clamp(target->Scroll.x - velocity_.x * step, 0.0f, target->ScrollMax.x));
    }
    if (vertical) {
        ImGui::SetScrollY(target, std::clamp(target->Scroll.y - velocity_.y * step, 0.0f, target->ScrollMax.y));
    }
    // How fast a glide loses its speed, per second.
    const float kept = std::exp(-4.0f * step);
    velocity_.x *= kept;
    velocity_.y *= kept;
    // Against an end of the content there is nowhere left to glide to.
    const ImVec2 wanted = target->ScrollTarget;
    const bool stuck = (!horizontal || wanted.x == before.x) && (!vertical || wanted.y == before.y);
    if (stuck) {
        scrolled_ = 0;
    }
    return !stuck;
}

void ImGuiBridge::Confine() {
    const ImGuiContext& context = *ImGui::GetCurrentContext();
    const ImVec2 screen = context.IO.DisplaySize;
    constexpr ImGuiWindowFlags PlacedByOthers = ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_Popup | ImGuiWindowFlags_NoMove;
    for (ImGuiWindow* window : context.Windows) {
        if (!window->WasActive || (window->Flags & PlacedByOthers) != 0) {
            continue;
        }
        const auto within = [](float position, float size, float room) { return size >= room ? 0.0f : std::clamp(position, 0.0f, room - size); };
        const ImVec2 kept(within(window->Pos.x, window->Size.x, screen.x), within(window->Pos.y, window->Size.y, screen.y));
        if (kept.x != window->Pos.x || kept.y != window->Pos.y) {
            ImGui::SetWindowPos(window, kept, ImGuiCond_Always);
        }
    }
}

void ImGuiBridge::Describe(InterfaceView& view, float margin) {
    const ImGuiContext& context = *ImGui::GetCurrentContext();
    view.regions.clear();
    for (const ImGuiWindow* window : context.Windows) {
        const bool child = (window->Flags & ImGuiWindowFlags_ChildWindow) != 0;
        const bool deaf = (window->Flags & ImGuiWindowFlags_NoMouseInputs) != 0;
        if (child || deaf || !window->Active || window->Hidden) {
            continue;
        }
        const ImRect area = window->Rect();
        view.regions.push_back({area.Min.x - margin, area.Min.y - margin, area.Max.x + margin, area.Max.y + margin});
    }
    view.unused.reset();
    view.dismissable = context.OpenPopupStack.Size > 0 || context.IO.WantTextInput;
    view.wantsKeys = context.IO.WantTextInput;
}
