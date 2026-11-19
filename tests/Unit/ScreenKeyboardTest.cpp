#include "Input/Keyboard/ScreenKeyboard.h"

#include "KeyboardTest.h"
#include "Test.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

class ScreenKeyboardTest : public KeyboardTest {
protected:
    static constexpr ImVec2 DisplaySize{1000.0f, 600.0f};

    static float Surface(const Area& area) {
        return area.GetWidth() * area.GetHeight();
    }

    // Whether `inner` lies inside `outer`, give or take `slack`.
    static bool Holds(const Area& outer, const Area& inner, float slack) {
        return inner.left >= outer.left - slack && inner.top >= outer.top - slack && inner.right <= outer.right + slack && inner.bottom <= outer.bottom + slack;
    }

    // Whether `one`, made smaller by `slack` on every side, still reaches into `other`.
    static bool Overlap(const Area& one, const Area& other, float slack) {
        return one.left + slack <= other.right && other.left <= one.right - slack && one.top + slack <= other.bottom && other.top <= one.bottom - slack;
    }

    struct Typist {
        ScreenKeyboard screen;
        Language language = Language::English;
        std::vector<Keystroke> events;

        Typist() {
            screen.Arrange(true, language, DisplaySize, 1.0f, 1.0f);
        }

        ImVec2 At(std::string_view name) const {
            return KeyAt(screen.GetKeys(), name);
        }

        void Finger(uint64_t touch, Phase phase, std::string_view name, double time) {
            screen.OnTouch(touch, phase, At(name), time, language, events);
        }

        void Tap(std::string_view name) {
            const ImVec2 position = At(name);
            for (const Phase phase : {Phase::Start, Phase::End}) {
                screen.OnTouch(0, phase, position, 0.0, language, events);
            }
        }

        std::u32string Text() const {
            return Typed(events);
        }

        size_t Presses(ImGuiKey wanted) const {
            return static_cast<size_t>(std::ranges::count_if(events, [wanted](const Keystroke& event) {
                const auto* key = std::get_if<Keystroke::Key>(&event.value);
                return key && key->key == wanted && key->pressed;
            }));
        }
    };
};

TEST(ScreenKeyboardTest, "every page tiles the keyboard without gaps or overlaps") {
    const Area bounds = {80.0f, 392.0f, 920.0f, 600.0f};
    struct Case {
        ScreenKeyboard::Page page;
        Language language;
        size_t count;
    };
    const Case pages[] = {
        {ScreenKeyboard::Page::Letters, Language::English, 34}, {ScreenKeyboard::Page::Letters, Language::Russian, 41}, {ScreenKeyboard::Page::Symbols, Language::English, 35}, {ScreenKeyboard::Page::More, Language::Russian, 35}, {ScreenKeyboard::Page::Edit, Language::Russian, 21},
    };
    for (const Case& page : pages) {
        const std::vector<ScreenKeyboard::Button> buttons = ScreenKeyboard::Layout(page.page, page.language, bounds);
        EXPECT_EQ(buttons.size(), page.count);
        float area = 0.0f;
        for (size_t index = 0; index < buttons.size(); index++) {
            const ScreenKeyboard::Button& button = buttons[index];
            EXPECT(Holds(bounds, button.rect, 0.01f) && button.rect.GetWidth() > 40.0f);
            for (size_t other = index + 1; other < buttons.size(); other++) {
                EXPECT(!Overlap(button.rect, buttons[other].rect, 0.01f));
            }
            area += Surface(button.rect);
        }
        const bool gap = page.page == ScreenKeyboard::Page::Letters && page.language == Language::English;
        EXPECT_NEAR(area + (gap ? 52.0f * 84.0f : 0.0f), Surface(bounds), 1.0);
    }
}

TEST(ScreenKeyboardTest, "the keyboard sits at the bottom and ignores zoom") {
    ScreenKeyboard screen;
    EXPECT_EQ(screen.Arrange(true, Language::English, DisplaySize, 1.0f, 1.0f), 208.0f);
    EXPECT(screen.GetBounds() == (Area{80.0f, 392.0f, 920.0f, 600.0f}));
    // The same display with everything shown twice as large: the keys keep their size.
    EXPECT_EQ(screen.Arrange(true, Language::English, DisplaySize, 2.0f, 2.0f), 208.0f);
    EXPECT(screen.GetBounds() == (Area{80.0f, 392.0f, 920.0f, 600.0f}));
    // A display twice as dense has twice the pixels under the same keys.
    EXPECT_EQ(screen.Arrange(true, Language::English, {2000.0f, 1200.0f}, 2.0f, 1.0f), 416.0f);
    EXPECT_EQ(screen.Arrange(true, Language::English, {640.0f, 360.0f}, 1.0f, 1.0f), 180.0f);
    EXPECT_EQ(screen.Arrange(false, Language::English, DisplaySize, 1.0f, 1.0f), 0.0f);
    EXPECT(!screen.GetBounds() && screen.GetKeys().empty());
}

TEST(ScreenKeyboardTest, "taps type text and shift applies once") {
    Typist typist;
    typist.Tap("shift");
    for (const char* key : {"H", "i", "space", ",", "enter"}) {
        typist.Tap(key);
    }
    EXPECT(typist.Text() == U"Hi ,");
    EXPECT_EQ(typist.Presses(ImGuiKey_Enter), size_t{1});

    typist.Tap("shift");
    typist.Tap("shift");
    typist.Tap("A");
    typist.Tap("B");
    EXPECT(typist.Text() == U"Hi ,AB");
}

TEST(ScreenKeyboardTest, "pages and languages switch") {
    Typist typist;
    for (const char* key : {"symbols", "7", "more", "€", "symbols", "language"}) {
        typist.Tap(key);
    }
    EXPECT(typist.language == Language::Russian);
    typist.Tap("shift");
    typist.Tap("Ж");
    typist.Tap("ё");
    EXPECT(typist.Text() == U"7€Жё");
}

TEST(ScreenKeyboardTest, "a second finger commits the first key and sliding picks the last one") {
    Typist typist;
    typist.Finger(1, Phase::Start, "a", 0.0);
    typist.Finger(2, Phase::Start, "b", 0.0);
    EXPECT(typist.Text() == U"a");
    typist.Finger(1, Phase::End, "a", 0.0);
    typist.Finger(2, Phase::Move, "n", 0.0);
    typist.Finger(2, Phase::End, "n", 0.0);
    EXPECT(typist.Text() == U"an");

    typist.Finger(3, Phase::Start, "q", 0.0);
    typist.screen.OnTouch(3, Phase::Cancel, {0.0f, 0.0f}, 0.0, typist.language, typist.events);
    typist.Finger(4, Phase::Start, "w", 0.0);
    typist.screen.OnTouch(4, Phase::End, {10.0f, 10.0f}, 0.0, typist.language, typist.events);
    EXPECT(typist.Text() == U"an");
}

TEST(ScreenKeyboardTest, "a held backspace repeats") {
    Typist typist;
    typist.Finger(0, Phase::Start, "backspace", 1.0);
    EXPECT_EQ(typist.Presses(ImGuiKey_Backspace), size_t{1});
    EXPECT(typist.screen.Tick(1.2, typist.language, typist.events) == 1.0 + Keyboard::RepeatDelay);
    const std::optional<double> next = typist.screen.Tick(1.5, typist.language, typist.events);
    EXPECT(next.has_value());
    typist.screen.Tick(next.value_or(0.0), typist.language, typist.events);
    EXPECT_EQ(typist.Presses(ImGuiKey_Backspace), size_t{3});
    typist.Finger(0, Phase::End, "backspace", 2.0);
    EXPECT(typist.screen.Tick(3.0, typist.language, typist.events) == std::nullopt);
    EXPECT_EQ(typist.Presses(ImGuiKey_Backspace), size_t{3});
}
