#include "Input/Keyboard/HardwareKeyboard.h"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>

using namespace std::string_view_literals;

constexpr uint16_t LeftCtrl = 29;
constexpr uint16_t LeftShift = 42;
constexpr uint16_t RightShift = 54;
constexpr uint16_t KeypadAsterisk = 55;
constexpr uint16_t LeftAlt = 56;
constexpr uint16_t KeySpace = 57;
constexpr uint16_t CapsLock = 58;
constexpr uint16_t KeypadEnter = 96;
constexpr uint16_t RightCtrl = 97;
constexpr uint16_t KeypadSlash = 98;
constexpr uint16_t RightAlt = 100;
constexpr uint16_t LeftMeta = 125;
constexpr uint16_t RightMeta = 126;
constexpr std::array<uint16_t, 8> ModifierKeys = {LeftShift, RightShift, LeftAlt, RightAlt, LeftCtrl, RightCtrl, LeftMeta, RightMeta};

// Characters by key code, plain and shifted, for each language.
constexpr std::array<std::array<std::u32string_view, 2>, 2> Layouts = {{
    {
        U"\0\0"
        U"1234567890-=\0\0qwertyuiop[]\0\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 "sv,
        U"\0\0!@#$%^&*()_+\0\0QWERTYUIOP{}\0\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 "sv,
    },
    {
        U"\0\0"
        U"1234567890-=\0\0йцукенгшщзхъ\0\0фывапролджэё\0\\ячсмитьбю.\0*\0 "sv,
        U"\0\0!\"№;%:?*()_+\0\0ЙЦУКЕНГШЩЗХЪ\0\0ФЫВАПРОЛДЖЭЁ\0/ЯЧСМИТЬБЮ,\0*\0 "sv,
    },
}};

// Every layout ends with the space bar, the last key that types anything.
static_assert(std::ranges::all_of(Layouts, [](const auto& layout) { return layout[0].size() == size_t{KeySpace} + 1 && layout[1].size() == size_t{KeySpace} + 1; }));

std::optional<ImGuiKey> HardwareKeyboard::ToImGuiKey(uint16_t code) {
    static constexpr ImGuiKey DigitKeys[] = {ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4, ImGuiKey_5, ImGuiKey_6, ImGuiKey_7, ImGuiKey_8, ImGuiKey_9, ImGuiKey_0};
    static constexpr ImGuiKey TopRow[] = {ImGuiKey_Q, ImGuiKey_W, ImGuiKey_E, ImGuiKey_R, ImGuiKey_T, ImGuiKey_Y, ImGuiKey_U, ImGuiKey_I, ImGuiKey_O, ImGuiKey_P};
    static constexpr ImGuiKey HomeRow[] = {ImGuiKey_A, ImGuiKey_S, ImGuiKey_D, ImGuiKey_F, ImGuiKey_G, ImGuiKey_H, ImGuiKey_J, ImGuiKey_K, ImGuiKey_L};
    static constexpr ImGuiKey BottomRow[] = {ImGuiKey_Z, ImGuiKey_X, ImGuiKey_C, ImGuiKey_V, ImGuiKey_B, ImGuiKey_N, ImGuiKey_M};
    static constexpr ImGuiKey FunctionKeys[] = {ImGuiKey_F1, ImGuiKey_F2, ImGuiKey_F3, ImGuiKey_F4, ImGuiKey_F5, ImGuiKey_F6, ImGuiKey_F7, ImGuiKey_F8, ImGuiKey_F9, ImGuiKey_F10};
    static constexpr ImGuiKey KeypadKeys[] = {ImGuiKey_Keypad7, ImGuiKey_Keypad8, ImGuiKey_Keypad9, ImGuiKey_KeypadSubtract, ImGuiKey_Keypad4, ImGuiKey_Keypad5, ImGuiKey_Keypad6, ImGuiKey_KeypadAdd, ImGuiKey_Keypad1, ImGuiKey_Keypad2, ImGuiKey_Keypad3, ImGuiKey_Keypad0, ImGuiKey_KeypadDecimal};

    if (Between(code, 2, 11)) {
        return DigitKeys[code - 2];
    }
    if (Between(code, 16, 25)) {
        return TopRow[code - 16];
    }
    if (Between(code, 30, 38)) {
        return HomeRow[code - 30];
    }
    if (Between(code, 44, 50)) {
        return BottomRow[code - 44];
    }
    if (Between(code, 59, 68)) {
        return FunctionKeys[code - 59];
    }
    if (Between(code, 71, 83)) {
        return KeypadKeys[code - 71];
    }
    switch (code) {
        case 1:
            return ImGuiKey_Escape;
        case 12:
            return ImGuiKey_Minus;
        case 13:
            return ImGuiKey_Equal;
        case 14:
            return ImGuiKey_Backspace;
        case 15:
            return ImGuiKey_Tab;
        case 26:
            return ImGuiKey_LeftBracket;
        case 27:
            return ImGuiKey_RightBracket;
        case 28:
            return ImGuiKey_Enter;
        case LeftCtrl:
            return ImGuiKey_LeftCtrl;
        case 39:
            return ImGuiKey_Semicolon;
        case 40:
            return ImGuiKey_Apostrophe;
        case 41:
            return ImGuiKey_GraveAccent;
        case LeftShift:
            return ImGuiKey_LeftShift;
        case 43:
            return ImGuiKey_Backslash;
        case 51:
            return ImGuiKey_Comma;
        case 52:
            return ImGuiKey_Period;
        case 53:
            return ImGuiKey_Slash;
        case RightShift:
            return ImGuiKey_RightShift;
        case KeypadAsterisk:
            return ImGuiKey_KeypadMultiply;
        case LeftAlt:
            return ImGuiKey_LeftAlt;
        case KeySpace:
            return ImGuiKey_Space;
        case CapsLock:
            return ImGuiKey_CapsLock;
        case 87:
            return ImGuiKey_F11;
        case 88:
            return ImGuiKey_F12;
        case KeypadEnter:
            return ImGuiKey_KeypadEnter;
        case RightCtrl:
            return ImGuiKey_RightCtrl;
        case KeypadSlash:
            return ImGuiKey_KeypadDivide;
        case RightAlt:
            return ImGuiKey_RightAlt;
        case 102:
            return ImGuiKey_Home;
        case 103:
            return ImGuiKey_UpArrow;
        case 104:
            return ImGuiKey_PageUp;
        case 105:
            return ImGuiKey_LeftArrow;
        case 106:
            return ImGuiKey_RightArrow;
        case 107:
            return ImGuiKey_End;
        case 108:
            return ImGuiKey_DownArrow;
        case 109:
            return ImGuiKey_PageDown;
        case 110:
            return ImGuiKey_Insert;
        case 111:
            return ImGuiKey_Delete;
        case LeftMeta:
            return ImGuiKey_LeftSuper;
        case RightMeta:
            return ImGuiKey_RightSuper;
        default:
            return std::nullopt;
    }
}

std::optional<char32_t> HardwareKeyboard::Character(uint16_t code, Language language, bool shift, bool caps) {
    const size_t index = code;
    char32_t character = 0;
    if (Between(code, 71, 83)) {
        character = U"789-456+1230."[index - 71];
    } else if (code == KeypadSlash) {
        character = U'/';
    } else {
        const auto& [plain, shifted] = Layouts[std::to_underlying(language)];
        if (index >= plain.size()) {
            return std::nullopt;
        }
        character = plain[index];
        if (shift != (caps && IsLowercase(character))) {
            character = shifted[index];
        }
    }
    if (character == 0) {
        return std::nullopt;
    }
    return character;
}

Modifiers HardwareKeyboard::GetModifiers() const {
    return modifiers_;
}

bool HardwareKeyboard::Press(uint16_t code, bool pressed, double time, Language& language, std::vector<Keystroke>& out) {
    if (const auto modifier = std::ranges::find(ModifierKeys, code); modifier != ModifierKeys.end()) {
        held_[static_cast<size_t>(modifier - ModifierKeys.begin())] = pressed;
        modifiers_ = {.alt = held_[2] || held_[3], .shift = held_[0] || held_[1], .command = held_[4] || held_[5] || held_[6] || held_[7]};
        out.push_back({Keystroke::ModifiersChanged{modifiers_}});
        if (pressed && modifiers_.alt && modifiers_.shift) {
            ToggleLanguage(language);
        }
    } else if (pressed) {
        if (code == CapsLock) {
            caps_ = !caps_;
        } else if (code == KeySpace && modifiers_.command) {
            ToggleLanguage(language);
            return true;
        }
        repeating_ = std::pair(code, time + RepeatDelay);
    } else if (repeating_ && repeating_->first == code) {
        repeating_.reset();
    }
    return Emit(code, pressed, false, language, out);
}

std::optional<double> HardwareKeyboard::Tick(double time, Language& language, std::vector<Keystroke>& out) {
    if (!repeating_) {
        return std::nullopt;
    }
    const auto [code, due] = *repeating_;
    if (due > time) {
        return due;
    }
    if (!Emit(code, true, true, language, out)) {
        repeating_.reset();
        return std::nullopt;
    }
    repeating_ = std::pair(code, time + RepeatInterval);
    return repeating_->second;
}

bool HardwareKeyboard::Emit(uint16_t code, bool pressed, bool repeat, Language language, std::vector<Keystroke>& out) const {
    const std::optional<ImGuiKey> key = HardwareKeyboard::ToImGuiKey(code);
    if (!key) {
        return false;
    }
    const Modifiers modifiers = modifiers_;
    if (pressed && modifiers.command) {
        switch (*key) {
            case ImGuiKey_C:
                out.push_back({Keystroke::Copy{}});
                return true;
            case ImGuiKey_X:
                out.push_back({Keystroke::Cut{}});
                return true;
            case ImGuiKey_V:
                out.push_back({Keystroke::Paste{}});
                return true;
            default:
                break;
        }
    }
    out.push_back({Keystroke::Key{*key, pressed, repeat, modifiers}});
    if (pressed && !modifiers.command && !modifiers.alt) {
        if (const std::optional<char32_t> typed = Character(code, language, modifiers.shift, caps_)) {
            out.push_back({Keystroke::Text{*typed}});
        }
    }
    return true;
}
