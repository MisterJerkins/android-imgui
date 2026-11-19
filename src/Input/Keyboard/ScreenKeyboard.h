#pragma once

#include "Input/Keyboard/Keyboard.h"
#include "Input/Touch/TouchParser.h"
#include "Input/Touch/TouchRouter.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The on-screen keyboard. It is painted over everything else and fed touches directly, so
// typing never moves the pointer and the text field keeps its focus.
class ScreenKeyboard final : public Keyboard {
public:
    enum class Page : uint8_t { Letters, Symbols, More, Edit };

    struct Cap {
        enum class Kind : uint8_t { Char, Space, Enter, Backspace, Shift, Symbols, More, Language, Key, Cut, Copy, Paste };

        Kind kind = Kind::Space;
        // What a `Char` types.
        char32_t character = 0;
        // A `Key` is a key press passed on as it is: moving the cursor, and shortcuts.
        ImGuiKey key = ImGuiKey_None;
        Modifiers modifiers;

        bool operator==(const Cap&) const = default;

        // A key that is neither a character nor a key press passed on.
        static constexpr Cap Plain(Kind kind) {
            return {kind, 0, ImGuiKey_None, {}};
        }

        static constexpr Cap Letter(char32_t character) {
            return {Kind::Char, character, ImGuiKey_None, {}};
        }

        static constexpr Cap Stroke(ImGuiKey key, Modifiers modifiers) {
            return {Kind::Key, 0, key, modifiers};
        }

        // Whether the key acts again and again while it is held.
        constexpr bool Repeats() const {
            return kind == Kind::Backspace || (kind == Kind::Key && modifiers.IsNone());
        }
    };

    struct Button {
        Area rect{};
        Cap cap;
    };

    // The keys of a page, laid out inside `bounds`.
    static std::vector<Button> Layout(Page page, Language language, const Area& bounds);

    std::optional<Area> GetBounds() const;

    // Whether the page with the clipboard keys is open.
    bool IsEditing() const;

    std::vector<std::pair<std::string, Area>> GetKeys() const;

    // Lays the keyboard out along the bottom edge of a display of `display` pixels and returns
    // its height, zero while hidden. `scale` is how many pixels a point takes and `zoom` the
    // user's share of it, which the keyboard does not follow: its keys stay finger-sized.
    float Arrange(bool shown, Language language, ImVec2 display, float scale, float zoom);

    void OnTouch(uint64_t id, Phase phase, ImVec2 position, double time, Language& language, std::vector<Keystroke>& out);

    // Repeats a held backspace or cursor key. Returns when it is due next.
    std::optional<double> Tick(double time, Language& language, std::vector<Keystroke>& out) override;

    void Paint() const;

private:
    enum class Case : uint8_t { Lower, Upper, Locked };

    struct Held {
        uint64_t id = 0;
        std::optional<size_t> button;
        std::optional<double> repeat;
        bool spent = false;
    };

    static void Icon(ImDrawList& list, const Cap& cap, ImVec2 center, float size, float width, ImU32 ink, bool locked);

    void Rebuild();
    std::optional<size_t> ButtonAt(ImVec2 position) const;
    void Release(size_t slot, Language& language, std::vector<Keystroke>& out);
    void Press(Cap cap, bool repeat, Language& language, std::vector<Keystroke>& out);
    char32_t Cased(char32_t character) const;
    // What a key goes by in `keys`.
    std::string Name(const Cap& cap) const;
    // What is written on a key. Keys without words on them get an icon.
    std::string Label(const Cap& cap) const;

    Page page_ = Page::Letters;
    Case case_ = Case::Lower;
    Language language_ = Language::English;
    std::optional<Area> bounds_;
    std::vector<Button> buttons_;
    std::vector<Held> held_;
};
