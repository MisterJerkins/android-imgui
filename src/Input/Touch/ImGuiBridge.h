#pragma once

#include "Input/Touch/TouchRouter.h"

#include "imgui.h"

struct ImGuiWindow;

// Hands routed input, given in display pixels, to Dear ImGui.
class ImGuiBridge {
public:
    // Passes a pointer event on. `false` for a key or a touch on the on-screen keyboard, which
    // are the keyboard's to handle.
    bool Feed(const InputEvent& event);

    // Lets a finger scroll a window by dragging its content, and keeps the content gliding
    // once the finger has lifted, as touch interfaces do; Dear ImGui by itself scrolls by its
    // scrollbars and the wheel. Call once a frame, right after `ImGui::NewFrame`. `true` while
    // something is still moving and frames should keep coming.
    bool Scroll();

    // Keeps every window the user can move whole on the screen, as far as it fits: dragged
    // past an edge it stops there, and a display that turns does not leave it outside. Call
    // right after `ImGui::NewFrame`.
    static void Confine();

    // What tells one frame's view from the next: which windows take touches, and whether a
    // touch outside them has something to dismiss. `margin` widens every window by that many
    // pixels, so its edges can be grabbed. Call after `ImGui::Render`.
    static void Describe(InterfaceView& view, float margin);

private:
    static int ButtonIndex(MouseButton button);

    static bool CanScroll(const ImGuiWindow& window, int axis);

    // The window a drag along `axis` scrolls: the one under the finger, or the nearest of its
    // parents that has somewhere to scroll to.
    static ImGuiWindow* Scrollable(ImGuiWindow* window, int axis);

    // Whether the drag may be turned into scrolling: nothing else is using it. A slider, a text
    // field and the like claim the arrow keys while they are held, which tells them from a
    // button that merely happens to be under the finger.
    static bool FreeToScroll(const ImGuiContext& context, ImGuiWindow& window, ImVec2 landed);

    // Fingers on the interface; while there are any, the pointer is a finger.
    int fingers_ = 0;
    bool touch_ = false;

    ImGuiID landed_ = 0;
    // Where the window under the finger was when it landed: a drag that turns out to be a
    // scroll puts it back there.
    ImVec2 resting_{};
    ImGuiID scrolled_ = 0;
    bool dragging_ = false;
    ImVec2 velocity_{};
};
