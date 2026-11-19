#pragma once

#include "Input/Keyboard/Keyboard.h"

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// Follows a hardware keyboard: modifiers, caps lock, shortcuts and key repeat.
class HardwareKeyboard final : public Keyboard {
public:
    // The key an evdev key code stands for.
    static std::optional<ImGuiKey> ToImGuiKey(uint16_t code);

    // The character a key types, if it types one.
    static std::optional<char32_t> Character(uint16_t code, Language language, bool shift, bool caps);

    Modifiers GetModifiers() const;

    // Returns whether the key means anything to the interface.
    bool Press(uint16_t code, bool pressed, double time, Language& language, std::vector<Keystroke>& out);

    // Repeats the held key. Returns when it is due next.
    std::optional<double> Tick(double time, Language& language, std::vector<Keystroke>& out) override;

private:
    static constexpr bool Between(uint16_t code, uint16_t first, uint16_t last) {
        return code >= first && code <= last;
    }

    bool Emit(uint16_t code, bool pressed, bool repeat, Language language, std::vector<Keystroke>& out) const;

    // Which of the modifier keys are down: both shifts, both alts, both controls, both metas.
    std::array<bool, 8> held_{};
    Modifiers modifiers_;
    bool caps_ = false;
    // The key that repeats, and when it does next.
    std::optional<std::pair<uint16_t, double>> repeating_;
};
