#pragma once

#include "Input/Touch/TouchRouter.h"

#include "imgui.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// A clipboard outside the interface, which copied text is shared with.
class Clipboard {
public:
    virtual ~Clipboard() = default;

    // A copy or a paste is likely soon; getting ready now saves the first one a wait.
    virtual void Prepare() {
    }
    // Called now and then, to give up what has not been needed for a while.
    virtual void Rest() {
    }
    virtual std::optional<std::string> Read() = 0;
    virtual void Write(std::string_view text) = 0;
};

enum class Language : uint8_t { English, Russian };

// The modifier keys that are down. `command` is Ctrl or Meta: either makes a shortcut, and
// Dear ImGui is told it as Ctrl.
struct Modifiers {
    bool alt = false;
    bool shift = false;
    bool command = false;

    bool operator==(const Modifiers&) const = default;

    constexpr bool IsNone() const {
        return !alt && !shift && !command;
    }
};

// What a keyboard has to say to the interface. The keyboards speak in these; `KeyboardManager`
// alone knows how Dear ImGui wants to be told.
struct Keystroke {
    struct ModifiersChanged {
        Modifiers modifiers;

        bool operator==(const ModifiersChanged&) const = default;
    };

    struct Key {
        ImGuiKey key;
        bool pressed;
        bool repeat;
        Modifiers modifiers;

        bool operator==(const Key&) const = default;
    };

    struct Text {
        char32_t character;

        bool operator==(const Text&) const = default;
    };

    struct Cut {
        bool operator==(const Cut&) const = default;
    };

    struct Copy {
        bool operator==(const Copy&) const = default;
    };

    // A paste is sent without its text, which Dear ImGui asks the clipboard's owner for.
    struct Paste {
        bool operator==(const Paste&) const = default;
    };

    std::variant<ModifiersChanged, Key, Text, Cut, Copy, Paste> value;

    bool operator==(const Keystroke&) const = default;
};

// A keyboard: it turns what the user does on it into keystrokes. `HardwareKeyboard` follows a
// real one, `ScreenKeyboard` is the one drawn on the screen; `KeyboardManager` hands what they
// say to Dear ImGui.
class Keyboard {
public:
    // How long a key is held before it starts to repeat, and how often it does then, in seconds.
    static constexpr double RepeatDelay = 0.4;
    static constexpr double RepeatInterval = 0.05;

    virtual ~Keyboard() = default;

    // Types again what is held down. Returns when that is due next.
    virtual std::optional<double> Tick(double time, Language& language, std::vector<Keystroke>& out) = 0;

protected:
    static constexpr void ToggleLanguage(Language& language) {
        language = language == Language::English ? Language::Russian : Language::English;
    }

    // Whether a character of the layouts is a small letter, the kind caps lock makes a capital of.
    static constexpr bool IsLowercase(char32_t character) {
        return (character >= U'a' && character <= U'z') || (character >= U'а' && character <= U'я') || character == U'ё';
    }

    // The capital of a letter of the layouts; anything else stays as it is.
    static constexpr char32_t Uppercase(char32_t character) {
        if (character >= U'a' && character <= U'z') {
            return static_cast<char32_t>(character - U'a' + U'A');
        }
        if (character >= U'а' && character <= U'я') {
            return static_cast<char32_t>(character - U'а' + U'А');
        }
        if (character == U'ё') {
            return U'Ё';
        }
        return character;
    }
};
