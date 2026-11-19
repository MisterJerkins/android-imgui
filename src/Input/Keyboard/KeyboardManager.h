#pragma once

#include "Input/Keyboard/HardwareKeyboard.h"
#include "Input/Keyboard/Keyboard.h"
#include "Input/Keyboard/ScreenKeyboard.h"
#include "Input/Touch/TouchParser.h"
#include "Input/Touch/TouchRouter.h"

#include "imgui.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Text entry: a hardware keyboard, plus an on-screen one that shows up while a text field has
// focus. There is one, `keyboard`, which `touch.Init` brings to life. Dear ImGui is handed a
// pointer to it, so it stays where it was made.
class KeyboardManager {
public:
    KeyboardManager() = default;
    KeyboardManager(const KeyboardManager&) = delete;
    KeyboardManager& operator=(const KeyboardManager&) = delete;
    ~KeyboardManager();

    Language GetLanguage() const;
    void SetLanguage(Language language);

    // Whether the on-screen keyboard may appear. A hardware keyboard works either way.
    void SetOnScreen(bool enabled);

    // The text on the clipboard: the system's where it can be asked, else what was last copied here.
    const std::string& GetClipboard();

    // Copies `text`, as the interface does on Ctrl+C.
    void SetClipboard(std::string text);

    void ShareClipboard(std::unique_ptr<Clipboard> shared);

    // Called now and then between frames.
    void Rest();

    // Where the on-screen keyboard is, in display pixels, while it is shown.
    std::optional<Area> GetRect() const;

    // The on-screen keys in display pixels, named by the character they type or by what they do.
    std::vector<std::pair<std::string, Area>> GetKeys() const;

    // The modifiers the hardware keyboard holds down.
    Modifiers GetModifiers() const;

    // How far up the frame on the screen is drawn from where Dear ImGui has its windows, in
    // pixels. It is nothing unless the line being typed would otherwise be under the keys.
    float GetLift() const;

    // What follows is called on the frame's thread, with the Dear ImGui context current, in
    // this order: `OnKey`, `OnTouch` and `ToInterface` for the input that came, `Tick`, `Arrange`,
    // then `ImGui::NewFrame`, the application's interface, `Paint`, `ImGui::Render`, `finish`
    // and `Describe`.

    // A key of a hardware keyboard, by its evdev code.
    void OnKey(uint16_t code, bool pressed, double time);

    // A touch that landed on the on-screen keyboard, in display pixels.
    void OnTouch(uint64_t id, Phase phase, ImVec2 position, double time);

    // A pointer's event as Dear ImGui has to be told it: the user points at what is drawn,
    // and while the interface is lifted Dear ImGui has that further down.
    InputEvent ToInterface(InputEvent event) const;

    // Key repeat. Returns when it is due next: a frame has to come by then.
    std::optional<double> Tick(double time);

    // Places the on-screen keyboard for the coming frame, on a display of `displaySize`
    // pixels. `scale` is how many pixels a point takes, the display's density times the
    // user's scale; `zoom` is the user's scale alone, which the keys do not follow.
    void Arrange(ImVec2 displaySize, float scale, float zoom);

    // Draws the on-screen keyboard over everything the frame has drawn.
    void Paint() const;

    // Takes what the frame asked of the platform. Returns whether another frame is due: the
    // on-screen keyboard has to appear or go, the interface has to move out of its way, or
    // Dear ImGui has not taken in everything that was typed.
    bool Finish();

    // Completes what `ImGuiBridge::Describe` told of the frame: where the keys are, and where the
    // windows are on the screen rather than where Dear ImGui has them.
    void Describe(InterfaceView& view) const;

private:
    static KeyboardManager& Of(ImGuiContext* context);
    // Tells Dear ImGui which modifiers are down. It drops what it already knows.
    static void Hold(Modifiers modifiers);
    // Whether Dear ImGui still has a press or a release of `key` to take in.
    static bool IsWaiting(ImGuiKey key);
    // Whether the pointer is pressed on the interface, or about to be.
    static bool IsPointing();

    void Attach();
    void Send(std::span<const Keystroke> strokes, bool hardware, bool likely);
    bool Raise(std::optional<float> line);
    void Shift(ImGuiContext& context);

    Language language_ = Language::English;
    HardwareKeyboard hardware_;
    ScreenKeyboard screen_;
    std::string clipboard_;
    std::unique_ptr<Clipboard> system_;
    bool onScreen_ = true;
    bool wanted_ = false;
    bool typed_ = false;
    // The text field the keyboard came up for.
    ImGuiID field_ = 0;
    // What Dear ImGui last said about the text cursor; it only says when something changes.
    ImGuiPlatformImeData ime_;
    float scale_ = 1.0f;
    // How far the interface is moved up to keep that field above the keys, in pixels.
    float lift_ = 0.0f;
    // How far up the frame now on the screen was drawn.
    float drawn_ = 0.0f;
    // What has Dear ImGui call back once a frame is ready to be drawn.
    ImGuiID hook_ = 0;
};

inline KeyboardManager keyboard;
