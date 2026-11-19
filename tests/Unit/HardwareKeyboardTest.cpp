#include "Input/Keyboard/HardwareKeyboard.h"

#include "KeyboardTest.h"
#include "Test.h"

#include <algorithm>
#include <string>
#include <vector>

class HardwareKeyboardTest : public KeyboardTest {
protected:
    static constexpr Language InEnglish = Language::English;
    static constexpr uint16_t LeftCtrl = 29;
    static constexpr uint16_t LeftShift = 42;
    static constexpr uint16_t KeySpace = 57;
    static constexpr uint16_t RightCtrl = 97;

    using Events = std::vector<Keystroke>;
};

TEST(HardwareKeyboardTest, "characters follow language shift and caps") {
    EXPECT(HardwareKeyboard::Character(30, InEnglish, false, false) == U'a');
    EXPECT(HardwareKeyboard::Character(30, InEnglish, true, false) == U'A');
    EXPECT(HardwareKeyboard::Character(30, InEnglish, false, true) == U'A');
    EXPECT(HardwareKeyboard::Character(30, InEnglish, true, true) == U'a');
    EXPECT(HardwareKeyboard::Character(2, InEnglish, true, true) == U'!');
    EXPECT(HardwareKeyboard::Character(2, InEnglish, false, true) == U'1');
    EXPECT(HardwareKeyboard::Character(53, InEnglish, true, false) == U'?');
    EXPECT(HardwareKeyboard::Character(57, InEnglish, false, false) == U' ');
    EXPECT(HardwareKeyboard::Character(79, InEnglish, false, false) == U'1');
    EXPECT(HardwareKeyboard::Character(28, InEnglish, false, false) == std::nullopt);
    EXPECT(HardwareKeyboard::Character(400, InEnglish, false, false) == std::nullopt);
    EXPECT(HardwareKeyboard::Character(16, Language::Russian, false, false) == U'й');
    EXPECT(HardwareKeyboard::Character(41, Language::Russian, true, false) == U'Ё');
    EXPECT(HardwareKeyboard::Character(52, Language::Russian, false, true) == U'Ю');
    EXPECT(HardwareKeyboard::Character(53, Language::Russian, true, false) == U',');
    EXPECT(HardwareKeyboard::ToImGuiKey(16) == ImGuiKey_Q);
    EXPECT(HardwareKeyboard::ToImGuiKey(50) == ImGuiKey_M);
    EXPECT(HardwareKeyboard::ToImGuiKey(103) == ImGuiKey_UpArrow);
    EXPECT(HardwareKeyboard::ToImGuiKey(999) == std::nullopt);
    // Every layout is 58 characters long: the space bar is its last key.
    for (const Language language : {InEnglish, Language::Russian}) {
        for (const bool shift : {false, true}) {
            EXPECT(HardwareKeyboard::Character(57, language, shift, false) == U' ');
            EXPECT(HardwareKeyboard::Character(58, language, shift, false) == std::nullopt);
        }
    }
}

TEST(HardwareKeyboardTest, "typing produces key and text events") {
    HardwareKeyboard keyboard;
    Language language = InEnglish;
    Events events;
    const std::pair<uint16_t, bool> keys[] = {{LeftShift, true}, {35, true}, {35, false}, {LeftShift, false}, {23, true}, {23, false}};
    for (const auto& [code, pressed] : keys) {
        EXPECT(keyboard.Press(code, pressed, 0.0, language, events));
    }
    EXPECT(Typed(events) == U"Hi");
    const auto* first = std::get_if<Keystroke::ModifiersChanged>(&events[0].value);
    EXPECT(first && first->modifiers.shift);
    EXPECT(std::ranges::any_of(events, [](const Keystroke& event) {
        const auto* key = std::get_if<Keystroke::Key>(&event.value);
        return key && key->key == ImGuiKey_H && key->pressed && key->modifiers.shift;
    }));
    EXPECT(!keyboard.Press(114, true, 0.0, language, events));
}

TEST(HardwareKeyboardTest, "a modifier stays down while either of its keys is held") {
    HardwareKeyboard keyboard;
    Language language = InEnglish;
    Events events;
    keyboard.Press(LeftCtrl, true, 0.0, language, events);
    keyboard.Press(RightCtrl, true, 0.0, language, events);
    keyboard.Press(LeftCtrl, false, 0.0, language, events);
    EXPECT(keyboard.GetModifiers().command);
    keyboard.Press(RightCtrl, false, 0.0, language, events);
    EXPECT(keyboard.GetModifiers() == Modifiers{});
}

TEST(HardwareKeyboardTest, "control shortcuts copy paste and switch language") {
    HardwareKeyboard keyboard;
    Language language = InEnglish;
    Events events;
    keyboard.Press(LeftCtrl, true, 0.0, language, events);
    keyboard.Press(47, true, 0.0, language, events);
    keyboard.Press(46, true, 0.0, language, events);
    EXPECT_EQ(events.size(), size_t{4});
    if (events.size() == 4) {
        EXPECT(events[2] == Keystroke{Keystroke::Paste{}});
        EXPECT(events[3] == Keystroke{Keystroke::Copy{}});
    }

    keyboard.Press(KeySpace, true, 0.0, language, events);
    keyboard.Press(LeftCtrl, false, 0.0, language, events);
    EXPECT(language == Language::Russian);
    events.clear();
    keyboard.Press(33, true, 0.0, language, events);
    EXPECT(Typed(events) == U"а");
}

TEST(HardwareKeyboardTest, "a held key repeats until released") {
    HardwareKeyboard keyboard;
    Language language = InEnglish;
    Events events;
    keyboard.Press(30, true, 1.0, language, events);
    EXPECT(keyboard.Tick(1.1, language, events) == 1.0 + Keyboard::RepeatDelay);
    EXPECT(Typed(events) == U"a");

    const std::optional<double> next = keyboard.Tick(1.0 + Keyboard::RepeatDelay, language, events);
    EXPECT(next.has_value());
    keyboard.Tick(next.value_or(0.0), language, events);
    EXPECT(Typed(events) == U"aaa");
    EXPECT(std::ranges::any_of(events, [](const Keystroke& event) {
        const auto* key = std::get_if<Keystroke::Key>(&event.value);
        return key && key->repeat && key->pressed;
    }));

    keyboard.Press(30, false, 2.0, language, events);
    EXPECT(keyboard.Tick(3.0, language, events) == std::nullopt);
    EXPECT(Typed(events) == U"aaa");
}
