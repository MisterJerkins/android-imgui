#pragma once

#include "Input/Keyboard/Keyboard.h"
#include "Test.h"

#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// What the suites of the keyboards share.
class KeyboardTest {
protected:
    // The text a run of keystrokes types.
    static std::u32string Typed(std::span<const Keystroke> events) {
        std::u32string text;
        for (const Keystroke& event : events) {
            if (const auto* typed = std::get_if<Keystroke::Text>(&event.value)) {
                text += typed->character;
            }
        }
        return text;
    }

    // The middle of the on-screen key that goes by `name`, out of the keys a keyboard lists.
    static ImVec2 KeyAt(const std::vector<std::pair<std::string, Area>>& keys, std::string_view name) {
        for (const auto& [key, rect] : keys) {
            if (key == name) {
                const auto [x, y] = rect.GetCenter();
                return {x, y};
            }
        }
        Test::Fail(__FILE__, __LINE__, std::format("no key '{}'", name));
        return {-1.0f, -1.0f};
    }
};
