#include "Input/Keyboard/ScreenKeyboard.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <initializer_list>
#include <span>
#include <string_view>

using Cap = ScreenKeyboard::Cap;
using Kind = ScreenKeyboard::Cap::Kind;

constexpr float RowCount = 4.0f;

constexpr Modifiers NoModifiers{};
// Ctrl, the way a hardware keyboard reports it.
constexpr Modifiers Command{.command = true};

void ScreenKeyboard::Icon(ImDrawList& list, const Cap& cap, ImVec2 center, float size, float width, ImU32 ink, bool locked) {
    using Point = std::array<float, 2>;
    const auto line = [&](std::initializer_list<Point> points, bool closed = false) {
        std::array<ImVec2, 8> path;
        size_t count = 0;
        for (const auto& [x, y] : points) {
            path[count++] = ImVec2(center.x + x * size, center.y + y * size);
        }
        list.AddPolyline(path.data(), static_cast<int>(count), ink, closed ? ImDrawFlags_Closed : ImDrawFlags_None, width);
    };
    switch (cap.kind) {
        case Kind::Shift:
            line({{0.0f, -1.0f}, {1.0f, 0.0f}, {0.45f, 0.0f}, {0.45f, 0.7f}, {-0.45f, 0.7f}, {-0.45f, 0.0f}, {-1.0f, 0.0f}}, true);
            if (locked) {
                line({{-0.45f, 1.1f}, {0.45f, 1.1f}});
            }
            break;
        case Kind::Backspace:
            line({{-1.3f, 0.0f}, {-0.5f, -0.8f}, {1.2f, -0.8f}, {1.2f, 0.8f}, {-0.5f, 0.8f}}, true);
            line({{0.0f, -0.35f}, {0.7f, 0.35f}});
            line({{0.7f, -0.35f}, {0.0f, 0.35f}});
            break;
        case Kind::Enter:
            line({{0.9f, -0.7f}, {0.9f, 0.2f}, {-0.9f, 0.2f}});
            line({{-0.45f, -0.25f}, {-0.9f, 0.2f}, {-0.45f, 0.65f}});
            break;
        case Kind::Space:
            line({{-2.0f, 0.0f}, {-2.0f, 0.4f}, {2.0f, 0.4f}, {2.0f, 0.0f}});
            break;
        case Kind::Key: {
            if (cap.key != ImGuiKey_LeftArrow && cap.key != ImGuiKey_RightArrow && cap.key != ImGuiKey_UpArrow && cap.key != ImGuiKey_DownArrow) {
                break;
            }
            const auto turn = [key = cap.key](float x, float y) -> Point {
                switch (key) {
                    case ImGuiKey_LeftArrow:
                        return {x, y};
                    case ImGuiKey_RightArrow:
                        return {-x, y};
                    case ImGuiKey_UpArrow:
                        return {y, x};
                    default:
                        return {y, -x};
                }
            };
            line({turn(0.9f, 0.0f), turn(-0.9f, 0.0f)});
            line({turn(-0.35f, -0.55f), turn(-0.9f, 0.0f), turn(-0.35f, 0.55f)});
            break;
        }
        default:
            break;
    }
}

std::vector<ScreenKeyboard::Button> ScreenKeyboard::Layout(Page page, Language language, const Area& bounds) {
    std::array<std::vector<Cap>, 4> rows;
    if (page == Page::Edit) {
        static constexpr Cap EditingTop[] = {Cap::Stroke(ImGuiKey_A, Command), Cap::Plain(Kind::Cut), Cap::Plain(Kind::Copy), Cap::Plain(Kind::Paste)};
        static constexpr Cap EditingMiddle[] = {
            Cap::Stroke(ImGuiKey_Home, NoModifiers), Cap::Stroke(ImGuiKey_LeftArrow, NoModifiers), Cap::Stroke(ImGuiKey_UpArrow, NoModifiers), Cap::Stroke(ImGuiKey_DownArrow, NoModifiers), Cap::Stroke(ImGuiKey_RightArrow, NoModifiers), Cap::Stroke(ImGuiKey_End, NoModifiers),
        };
        static constexpr Cap EditingBottom[] = {Cap::Stroke(ImGuiKey_Z, Command), Cap::Stroke(ImGuiKey_Z, Modifiers{.shift = true, .command = true}), Cap::Stroke(ImGuiKey_Delete, NoModifiers)};
        static constexpr std::array<std::span<const Cap>, 3> EditingRows = {EditingTop, EditingMiddle, EditingBottom};
        for (size_t row = 0; row < EditingRows.size(); row++) {
            rows[row].assign(EditingRows[row].begin(), EditingRows[row].end());
        }
    } else {
        static constexpr std::array<std::u32string_view, 3> EnglishRows = {U"qwertyuiop", U"asdfghjkl", U"zxcvbnm"};
        static constexpr std::array<std::u32string_view, 3> RussianRows = {U"йцукенгшщзхъ", U"фывапролджэё", U"ячсмитьбю"};
        static constexpr std::array<std::u32string_view, 3> SymbolRows = {U"1234567890", U"@#$%&-+()/", U"*\"':;!?"};
        static constexpr std::array<std::u32string_view, 3> MoreSymbolRows = {U"~`|^=_{}[]", U"<>\\€£¥°•√π", U"÷×§©®™…"};
        const auto& letters = page == Page::Symbols ? SymbolRows : page == Page::More ? MoreSymbolRows : language == Language::English ? EnglishRows : RussianRows;
        for (size_t row = 0; row < letters.size(); row++) {
            for (const char32_t character : letters[row]) {
                rows[row].push_back(Cap::Letter(character));
            }
        }
    }
    rows[2].insert(rows[2].begin(), Cap::Plain(page == Page::Letters ? Kind::Shift : Kind::More));
    rows[2].push_back(Cap::Plain(Kind::Backspace));
    rows[3] = {Cap::Plain(Kind::Symbols), Cap::Plain(Kind::Language), Cap::Letter(U','), Cap::Plain(Kind::Space), Cap::Letter(U'.'), Cap::Plain(Kind::Enter)};

    // A key that is no letter is half as wide again; the space takes what is left.
    const auto weight = [](const Cap& key) { return key.kind == Kind::Char ? 1.0f : key.kind == Kind::Space ? 0.0f : 1.5f; };
    const auto total = [&weight](const std::vector<Cap>& row) {
        float sum = 0.0f;
        for (const Cap& key : row) {
            sum += weight(key);
        }
        return sum;
    };
    float units = 0.0f;
    for (const std::vector<Cap>& row : rows) {
        units = std::max(units, total(row));
    }
    const float unit = bounds.GetWidth() / units;
    const float height = bounds.GetHeight() / RowCount;

    std::vector<Button> buttons;
    for (size_t index = 0; index < rows.size(); index++) {
        const std::vector<Cap>& row = rows[index];
        const float spare = units - total(row);
        // A row is filled by its space bar; without one it is centred, or on the editing page,
        // which has few keys, shared out between them.
        const bool spaced = std::ranges::any_of(row, [](const Cap& key) { return key.kind == Kind::Space; });
        const float grow = !spaced && page == Page::Edit ? units / total(row) : 1.0f;
        const float margin = !spaced && page != Page::Edit ? spare * unit / 2.0f : 0.0f;
        const float top = bounds.top + static_cast<float>(index) * height;
        float left = bounds.left + margin;
        for (const Cap& key : row) {
            const float width = unit * (key.kind == Kind::Space ? spare : weight(key) * grow);
            buttons.push_back({{left, top, left + width, top + height}, key});
            left += width;
        }
    }
    return buttons;
}

std::optional<Area> ScreenKeyboard::GetBounds() const {
    return bounds_;
}

bool ScreenKeyboard::IsEditing() const {
    return page_ == Page::Edit;
}

std::vector<std::pair<std::string, Area>> ScreenKeyboard::GetKeys() const {
    std::vector<std::pair<std::string, Area>> named;
    named.reserve(buttons_.size());
    for (const Button& button : buttons_) {
        named.emplace_back(Name(button.cap), button.rect);
    }
    return named;
}

float ScreenKeyboard::Arrange(bool shown, Language language, ImVec2 display, float scale, float zoom) {
    if (!shown) {
        *this = ScreenKeyboard();
        return 0.0f;
    }
    // 840 points wide at most and 52 points to a row, but never more than half the screen high.
    const float width = std::min(840.0f / zoom * scale, display.x);
    const float height = std::min(RowCount * 52.0f / zoom * scale, display.y / 2.0f);
    const float left = display.x / 2.0f - width / 2.0f;
    const Area bounds = {left, display.y - height, left + width, display.y};
    if (bounds_ != bounds || language_ != language) {
        bounds_ = bounds;
        language_ = language;
        Rebuild();
    }
    return height;
}

void ScreenKeyboard::OnTouch(uint64_t id, Phase phase, ImVec2 position, double time, Language& language, std::vector<Keystroke>& out) {
    const std::optional<size_t> under = ButtonAt(position);
    const auto found = std::ranges::find(held_, id, &Held::id);
    const auto slot = found - held_.begin();
    switch (phase) {
        case Phase::Start: {
            for (size_t other = 0; other < held_.size(); other++) {
                Release(other, language, out);
            }
            const std::optional<Cap> cap = under ? std::optional(buttons_[*under].cap) : std::nullopt;
            const bool instant = cap && cap->kind != Kind::Char && cap->kind != Kind::Space && cap->kind != Kind::Enter;
            if (instant) {
                Press(*cap, false, language, out);
            }
            const std::optional<double> repeat = cap && cap->Repeats() ? std::optional(time + RepeatDelay) : std::nullopt;
            held_.push_back({id, under, repeat, instant});
            break;
        }
        case Phase::Move:
            if (found != held_.end() && !found->spent) {
                found->button = under;
            }
            break;
        case Phase::End:
            if (found == held_.end()) {
                break;
            }
            if (!found->spent) {
                found->button = under;
            }
            Release(static_cast<size_t>(slot), language, out);
            held_.erase(held_.begin() + slot);
            break;
        case Phase::Cancel:
            if (found != held_.end()) {
                held_.erase(found);
            }
            break;
    }
}

std::optional<double> ScreenKeyboard::Tick(double time, Language& language, std::vector<Keystroke>& out) {
    std::optional<double> next;
    for (Held& held : held_) {
        if (!held.repeat || !held.button || *held.button >= buttons_.size()) {
            continue;
        }
        double due = *held.repeat;
        if (due <= time) {
            due = time + RepeatInterval;
            held.repeat = due;
            Press(buttons_[*held.button].cap, true, language, out);
        }
        next = next ? std::min(*next, due) : due;
    }
    return next;
}

void ScreenKeyboard::Paint() const {
    if (!bounds_) {
        return;
    }
    const Area& bounds = *bounds_;
    ImDrawList& list = *ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float unit = bounds.GetHeight() / RowCount;
    const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
    // The keys hide what they cover, however much the theme's windows let through.
    list.AddRectFilled({bounds.left, bounds.top}, {bounds.right, bounds.bottom}, ImGui::GetColorU32(ImGuiCol_WindowBg) | IM_COL32_A_MASK);
    for (size_t index = 0; index < buttons_.size(); index++) {
        const Button& button = buttons_[index];
        const bool held = std::ranges::any_of(held_, [index](const Held& finger) { return finger.button == index; });
        const bool lit = held || (button.cap.kind == Kind::Shift && case_ != Case::Lower);
        const float inset = unit * 0.06f;
        const ImVec2 min(button.rect.left + inset, button.rect.top + inset);
        const ImVec2 max(button.rect.right - inset, button.rect.bottom - inset);
        list.AddRectFilled(min, max, ImGui::GetColorU32(lit ? ImGuiCol_ButtonActive : ImGuiCol_Button), unit * 0.14f);

        const auto [middle, level] = button.rect.GetCenter();
        const ImVec2 center(middle, level);
        const std::string text = Label(button.cap);
        if (text.empty()) {
            Icon(list, button.cap, center, unit * 0.2f, unit * 0.045f, ink, case_ == Case::Locked);
            continue;
        }
        const char* begin = text.data();
        const char* end = begin + text.size();
        // A word is written smaller than a single character. In UTF-8 every byte but a continuation starts one.
        const bool word = std::ranges::count_if(text, [](char byte) { return (static_cast<unsigned char>(byte) & 0xC0) != 0x80; }) > 1;
        float size = unit * (word ? 0.3f : 0.42f);
        const float room = (max.x - min.x) * 0.9f;
        ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, begin, end);
        if (extent.x > room) {
            size *= room / extent.x;
            extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, begin, end);
        }
        list.AddText(font, size, {center.x - extent.x / 2.0f, center.y - extent.y / 2.0f}, ink, begin, end);
    }
}

void ScreenKeyboard::Rebuild() {
    buttons_ = bounds_ ? Layout(page_, language_, *bounds_) : std::vector<Button>();
    for (Held& held : held_) {
        held.button.reset();
    }
}

std::optional<size_t> ScreenKeyboard::ButtonAt(ImVec2 position) const {
    // A touch on the very edge of the keyboard is still on it.
    const std::array<float, 2> point = {position.x, position.y};
    if (!bounds_ || bounds_->DistanceSq(point) != 0.0f) {
        return std::nullopt;
    }
    std::optional<size_t> nearest;
    float least = 0.0f;
    for (size_t index = 0; index < buttons_.size(); index++) {
        const float distance = buttons_[index].rect.DistanceSq(point);
        if (!nearest || distance < least) {
            nearest = index;
            least = distance;
        }
    }
    return nearest;
}

void ScreenKeyboard::Release(size_t slot, Language& language, std::vector<Keystroke>& out) {
    Held& held = held_[slot];
    if (std::exchange(held.spent, true)) {
        return;
    }
    if (held.button && *held.button < buttons_.size()) {
        Press(buttons_[*held.button].cap, false, language, out);
    }
}

void ScreenKeyboard::Press(Cap cap, bool repeat, Language& language, std::vector<Keystroke>& out) {
    const auto strike = [&out, repeat](ImGuiKey key, Modifiers modifiers) {
        out.push_back({Keystroke::Key{key, true, repeat, modifiers}});
        out.push_back({Keystroke::Key{key, false, false, modifiers}});
    };
    switch (cap.kind) {
        case Kind::Enter:
            strike(ImGuiKey_Enter, NoModifiers);
            break;
        case Kind::Backspace:
            strike(ImGuiKey_Backspace, NoModifiers);
            break;
        case Kind::Key:
            strike(cap.key, cap.modifiers);
            break;
        case Kind::Cut:
            out.push_back({Keystroke::Cut{}});
            break;
        case Kind::Copy:
            out.push_back({Keystroke::Copy{}});
            break;
        case Kind::Paste:
            out.push_back({Keystroke::Paste{}});
            break;
        case Kind::Space:
            out.push_back({Keystroke::Text{U' '}});
            break;
        case Kind::Char:
            out.push_back({Keystroke::Text{Cased(cap.character)}});
            if (case_ == Case::Upper) {
                case_ = Case::Lower;
            }
            break;
        case Kind::Shift:
            case_ = case_ == Case::Lower ? Case::Upper : case_ == Case::Upper ? Case::Locked : Case::Lower;
            break;
        case Kind::Symbols:
        case Kind::More:
        case Kind::Language:
            if (cap.kind == Kind::Symbols) {
                page_ = page_ == Page::Letters ? Page::Symbols : Page::Letters;
            } else if (cap.kind == Kind::More) {
                page_ = page_ == Page::Symbols ? Page::More : page_ == Page::More ? Page::Edit : Page::Symbols;
            } else {
                ToggleLanguage(language);
            }
            language_ = language;
            Rebuild();
            break;
    }
}

char32_t ScreenKeyboard::Cased(char32_t character) const {
    return case_ == Case::Lower || page_ != Page::Letters ? character : Uppercase(character);
}

std::string ScreenKeyboard::Name(const Cap& cap) const {
    switch (cap.kind) {
        case Kind::Char:
            return Utils::ToUtf8(Cased(cap.character));
        case Kind::Space:
            return "space";
        case Kind::Enter:
            return "enter";
        case Kind::Backspace:
            return "backspace";
        case Kind::Shift:
            return "shift";
        case Kind::Symbols:
            return "symbols";
        case Kind::More:
            return "more";
        case Kind::Language:
            return "language";
        case Kind::Key:
            switch (cap.key) {
                case ImGuiKey_A:
                    return "all";
                case ImGuiKey_Z:
                    return cap.modifiers.shift ? "redo" : "undo";
                case ImGuiKey_Home:
                    return "home";
                case ImGuiKey_LeftArrow:
                    return "left";
                case ImGuiKey_UpArrow:
                    return "up";
                case ImGuiKey_DownArrow:
                    return "down";
                case ImGuiKey_RightArrow:
                    return "right";
                case ImGuiKey_End:
                    return "end";
                case ImGuiKey_Delete:
                    return "delete";
                default:
                    return "";
            }
        case Kind::Cut:
            return "cut";
        case Kind::Copy:
            return "copy";
        case Kind::Paste:
            return "paste";
    }
    return "";
}

std::string ScreenKeyboard::Label(const Cap& cap) const {
    const auto words = [this](const char* english, const char* russian) { return language_ == Language::English ? english : russian; };
    switch (cap.kind) {
        case Kind::Char:
            return Utils::ToUtf8(Cased(cap.character));
        case Kind::Symbols:
            return page_ == Page::Letters ? "?123" : words("ABC", "АБВ");
        case Kind::More:
            return page_ == Page::Symbols ? "1/3" : page_ == Page::More ? "2/3" : "3/3";
        case Kind::Language:
            return words("EN", "RU");
        case Kind::Key:
            switch (cap.key) {
                case ImGuiKey_A:
                    return words("Select all", "Выделить всё");
                case ImGuiKey_Z:
                    return cap.modifiers.shift ? words("Redo", "Вернуть") : words("Undo", "Отменить");
                case ImGuiKey_Home:
                    return "Home";
                case ImGuiKey_End:
                    return "End";
                case ImGuiKey_Delete:
                    return "Del";
                default:
                    return "";
            }
        case Kind::Cut:
            return words("Cut", "Вырезать");
        case Kind::Copy:
            return words("Copy", "Копировать");
        case Kind::Paste:
            return words("Paste", "Вставить");
        case Kind::Space:
        case Kind::Enter:
        case Kind::Backspace:
        case Kind::Shift:
            return "";
    }
    return "";
}
