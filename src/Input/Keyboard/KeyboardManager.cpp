#include "Input/Keyboard/KeyboardManager.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <variant>

void KeyboardManager::Hold(Modifiers modifiers) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, modifiers.command);
    io.AddKeyEvent(ImGuiMod_Shift, modifiers.shift);
    io.AddKeyEvent(ImGuiMod_Alt, modifiers.alt);
}

bool KeyboardManager::IsWaiting(ImGuiKey key) {
    const ImGuiContext& context = *ImGui::GetCurrentContext();
    return std::ranges::any_of(context.InputEventsQueue, [key](const ImGuiInputEvent& event) { return event.Type == ImGuiInputEventType_Key && event.Key.Key == key; });
}

bool KeyboardManager::IsPointing() {
    const ImGuiContext& context = *ImGui::GetCurrentContext();
    const bool down = std::ranges::any_of(context.IO.MouseDown, [](bool button) { return button; });
    return down || std::ranges::any_of(context.InputEventsQueue, [](const ImGuiInputEvent& event) { return event.Type == ImGuiInputEventType_MouseButton; });
}

KeyboardManager::~KeyboardManager() {
    ImGuiContext* context = ImGui::GetCurrentContext();
    if (!context || context->PlatformIO.Platform_ClipboardUserData != this) {
        return;
    }
    ImGuiPlatformIO& platform = context->PlatformIO;
    platform.Platform_GetClipboardTextFn = nullptr;
    platform.Platform_SetClipboardTextFn = nullptr;
    platform.Platform_SetImeDataFn = nullptr;
    platform.Platform_ClipboardUserData = nullptr;
    ImGui::RemoveContextHook(context, hook_);
}

Language KeyboardManager::GetLanguage() const {
    return language_;
}

void KeyboardManager::SetLanguage(Language language) {
    language_ = language;
}

void KeyboardManager::SetOnScreen(bool enabled) {
    onScreen_ = enabled;
}

const std::string& KeyboardManager::GetClipboard() {
    if (system_) {
        if (std::optional<std::string> text = system_->Read()) {
            clipboard_ = std::move(*text);
        }
    }
    return clipboard_;
}

void KeyboardManager::SetClipboard(std::string text) {
    clipboard_ = std::move(text);
    if (system_) {
        system_->Write(clipboard_);
    }
}

void KeyboardManager::ShareClipboard(std::unique_ptr<Clipboard> shared) {
    system_ = std::move(shared);
}

void KeyboardManager::Rest() {
    if (system_) {
        system_->Rest();
    }
}

std::optional<Area> KeyboardManager::GetRect() const {
    return screen_.GetBounds();
}

std::vector<std::pair<std::string, Area>> KeyboardManager::GetKeys() const {
    return screen_.GetKeys();
}

Modifiers KeyboardManager::GetModifiers() const {
    return hardware_.GetModifiers();
}

float KeyboardManager::GetLift() const {
    return drawn_;
}

void KeyboardManager::OnKey(uint16_t code, bool pressed, double time) {
    std::vector<Keystroke> strokes;
    const bool used = hardware_.Press(code, pressed, time, language_, strokes);
    typed_ |= used && pressed && wanted_;
    Send(strokes, true, hardware_.GetModifiers().command);
}

void KeyboardManager::OnTouch(uint64_t id, Phase phase, ImVec2 position, double time) {
    std::vector<Keystroke> strokes;
    screen_.OnTouch(id, phase, position, time, language_, strokes);
    Send(strokes, false, screen_.IsEditing());
}

InputEvent KeyboardManager::ToInterface(InputEvent event) const {
    const float down = drawn_;
    const Utils::Overloaded lower{
        [down](InputEvent::Touch& finger) { finger.position[1] += down; },
        [down](InputEvent::PointerMoved& moved) { moved.position[1] += down; },
        [down](InputEvent::PointerButton& button) { button.position[1] += down; },
        [](auto&) {},
    };
    std::visit(lower, event.value);
    return event;
}

std::optional<double> KeyboardManager::Tick(double time) {
    std::vector<Keystroke> strokes;
    const std::optional<double> physical = hardware_.Tick(time, language_, strokes);
    Send(strokes, true, false);
    strokes.clear();
    const std::optional<double> screen = screen_.Tick(time, language_, strokes);
    Send(strokes, false, false);
    if (physical && screen) {
        return std::min(*physical, *screen);
    }
    return physical ? physical : screen;
}

void KeyboardManager::Arrange(ImVec2 displaySize, float scale, float zoom) {
    Attach();
    scale_ = scale;
    screen_.Arrange(onScreen_ && wanted_ && !typed_, language_, displaySize, scale, zoom);
}

void KeyboardManager::Paint() const {
    screen_.Paint();
}

bool KeyboardManager::Finish() {
    const ImGuiContext& context = *ImGui::GetCurrentContext();
    // A field asks for text only in the frames it is drawn in; `io.WantTextInput` says the same
    // a frame later. Once the keyboard is up for one, it stays for as long as that field keeps
    // the focus, seen or not.
    const ImGuiID focused = context.ActiveId;
    if (ime_.WantTextInput) {
        field_ = focused;
    }
    const bool wanted = field_ != 0 && field_ == focused;
    const bool changed = wanted != wanted_;
    if (changed) {
        wanted_ = wanted;
        typed_ = false;
    }
    if (!wanted) {
        field_ = 0;
    }
    const bool moved = Raise(ime_.WantVisible ? std::optional(ime_.InputPos.y + ime_.InputLineHeight) : std::nullopt);
    // Dear ImGui takes a key's press and its release in separate frames.
    const bool behind = context.InputEventsQueue.Size > 0;
    return changed || moved || behind || screen_.GetBounds().has_value() != (onScreen_ && wanted_ && !typed_);
}

void KeyboardManager::Describe(InterfaceView& view) const {
    view.keyboard = screen_.GetBounds();
    for (Area& region : view.regions) {
        region.top -= drawn_;
        region.bottom -= drawn_;
    }
}

KeyboardManager& KeyboardManager::Of(ImGuiContext* context) {
    return *static_cast<KeyboardManager*>(context->PlatformIO.Platform_ClipboardUserData);
}

// Makes this the keyboard Dear ImGui asks for the clipboard, tells where its text cursor is,
// and hands every frame to before it is drawn.
void KeyboardManager::Attach() {
    ImGuiContext& context = *ImGui::GetCurrentContext();
    ImGuiPlatformIO& platform = context.PlatformIO;
    if (platform.Platform_ClipboardUserData == this) {
        return;
    }
    platform.Platform_ClipboardUserData = this;
    // What was kept of a context that is gone says nothing about this one.
    wanted_ = typed_ = false;
    field_ = 0;
    lift_ = drawn_ = 0.0f;
    platform.Platform_GetClipboardTextFn = [](ImGuiContext* from) { return Of(from).GetClipboard().c_str(); };
    platform.Platform_SetClipboardTextFn = [](ImGuiContext* from, const char* text) { Of(from).SetClipboard(text); };
    platform.Platform_SetImeDataFn = [](ImGuiContext* from, ImGuiViewport*, ImGuiPlatformImeData* data) { Of(from).ime_ = *data; };
    ime_ = context.PlatformImeData;

    ImGuiContextHook hook;
    hook.Type = ImGuiContextHookType_RenderPost;
    hook.Callback = [](ImGuiContext* from, ImGuiContextHook*) { Of(from).Shift(*from); };
    hook_ = ImGui::AddContextHook(&context, &hook);

    // Dear ImGui repeats a key that stays down by itself; it does so at the pace kept here.
    context.IO.KeyRepeatDelay = static_cast<float>(Keyboard::RepeatDelay);
    context.IO.KeyRepeatRate = static_cast<float>(Keyboard::RepeatInterval);
}

// Hands Dear ImGui what a keyboard said. The keys of a hardware keyboard stay down until they
// are let go; those of the on-screen one go down and up at once. A paste has Dear ImGui ask
// for the clipboard's text when it gets to it; without one among `strokes`, `likely` says a
// copy or paste may come next.
void KeyboardManager::Send(std::span<const Keystroke> strokes, bool hardware, bool likely) {
    ImGuiIO& io = ImGui::GetIO();
    bool pasted = false;
    const auto shortcut = [&io, hardware](ImGuiKey key) {
        Hold({.command = true});
        io.AddKeyEvent(key, true);
        if (!hardware) {
            io.AddKeyEvent(key, false);
        }
    };
    const Utils::Overloaded pass{
        [](const Keystroke::ModifiersChanged& changed) { Hold(changed.modifiers); },
        [&io, hardware](const Keystroke::Key& key) {
            // Dear ImGui repeats a hardware key itself, for it knows the key is
            // down. An on-screen key comes down anew each time, and a time that
            // Dear ImGui has not caught up with is dropped: queued, a key held
            // through slow frames would go on long after it was let go.
            if (key.repeat && (hardware || IsWaiting(key.key))) {
                return;
            }
            Hold(key.modifiers);
            io.AddKeyEvent(key.key, key.pressed);
        },
        [&io](const Keystroke::Text& text) { io.AddInputCharacter(text.character); },
        [&shortcut](const Keystroke::Cut&) { shortcut(ImGuiKey_X); },
        [&shortcut](const Keystroke::Copy&) { shortcut(ImGuiKey_C); },
        [&shortcut, &pasted](const Keystroke::Paste&) {
            pasted = true;
            shortcut(ImGuiKey_V);
        },
    };
    for (const Keystroke& stroke : strokes) {
        std::visit(pass, stroke.value);
    }
    if (!strokes.empty()) {
        Hold(hardware_.GetModifiers());
    }
    if (!pasted && likely && system_) {
        system_->Prepare();
    }
}

// Moves the interface up by as much as the line being typed is under the keys, and back once
// it need not be. `line` is the bottom of that line, if it was drawn. Returns whether
// anything moved.
bool KeyboardManager::Raise(std::optional<float> line) {
    // Dear ImGui acts on a press where the pointer went down and on a release where it comes
    // up, and a field gives up its focus on the press: what is under a finger stays under it.
    if (IsPointing()) {
        return false;
    }
    const std::optional<Area> keys = screen_.GetBounds();
    float lift = 0.0f;
    if (keys && line) {
        // The line is reported where Dear ImGui has it, which is before the lift.
        // Room kept between the line being typed and the keys under it, in points.
        const float needed = std::max(*line + 8.0f * scale_ - keys->top, 0.0f);
        lift = std::fabs(needed - lift_) <= 1.0f ? lift_ : std::round(needed);
    } else if (keys) {
        lift = lift_;
    }
    const bool moved = lift != lift_;
    lift_ = lift;
    return moved;
}

// Moves the windows of the frame just made up by the lift, in what is drawn of them alone.
// Moving the windows themselves would not do: Dear ImGui keeps every window on the display,
// and one the application places is put back each frame. To Dear ImGui they stay where they
// were, which is why the pointer is told to it through `ToInterface`. What is drawn behind
// and in front of every window, the keys among it, is left in place.
void KeyboardManager::Shift(ImGuiContext& context) {
    drawn_ = lift_;
    if (drawn_ == 0.0f) {
        return;
    }
    const ImGuiViewportP& viewport = *context.Viewports[0];
    for (ImDrawList* list : viewport.DrawDataP.CmdLists) {
        if (list == viewport.BgFgDrawLists[0] || list == viewport.BgFgDrawLists[1]) {
            continue;
        }
        for (ImDrawVert& vertex : list->VtxBuffer) {
            vertex.pos.y -= drawn_;
        }
        for (ImDrawCmd& command : list->CmdBuffer) {
            command.ClipRect.y -= drawn_;
            command.ClipRect.w -= drawn_;
        }
    }
}
