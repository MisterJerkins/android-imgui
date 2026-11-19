#include "Input/Keyboard/KeyboardManager.h"

#include "Input/Touch/ImGuiBridge.h"
#include "KeyboardTest.h"
#include "Test.h"

#include "imgui_internal.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class KeyboardManagerTest : public KeyboardTest {
protected:
    static constexpr ImVec2 DisplaySize{1000.0f, 600.0f};
    // Something drawn behind every window, as the application's own drawing is.
    static constexpr ImRect MarkBehind{10.0f, 500.0f, 30.0f, 520.0f};

    // A Dear ImGui context that draws nowhere, set up the way the overlay sets its own up.
    struct Context {
        Context() {
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
            io.ConfigWindowsMoveFromTitleBarOnly = true;
        }
        ~Context() {
            ImGui::DestroyContext();
        }
    };

    static ImRect Bounds(const ImDrawList& list) {
        ImRect box(1e9f, 1e9f, -1e9f, -1e9f);
        for (const ImDrawVert& vertex : list.VtxBuffer) {
            box.Add(vertex.pos);
        }
        return box;
    }

    // A window with a text field and a button, and input handed to it the way the overlay does.
    struct Harness {
        Context context;
        KeyboardManager keyboard;
        TouchRouter router;
        ImGuiBridge bridge;
        InterfaceView view;
        char text[64] = "";
        int clicks = 0;
        double time = 0.0;
        // How long a frame takes.
        double step = 1.0 / 60.0;
        // How far down its window the text field sits; zero puts it at the top of the screen.
        float depth = 0.0f;
        // Where the field and the button were drawn last, on screen.
        ImRect field;
        ImRect button;
        int frames = 0;

        // A text field that takes focus on the first frame, run until the keyboard has settled.
        Harness() : Harness(0.0f) {
            EXPECT(!Frame() && !keyboard.GetRect());
            EXPECT(Frame());
            EXPECT(!Frame());
        }

        explicit Harness(float fieldDepth) : depth(fieldDepth) {
            view.size = {DisplaySize.x, DisplaySize.y};
        }

        bool Frame() {
            time += step;
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = DisplaySize;
            io.DeltaTime = static_cast<float>(step);
            keyboard.Tick(time);
            keyboard.Arrange(DisplaySize, 1.0f, 1.0f);
            ImGui::NewFrame();
            ImGuiBridge::Confine();
            bridge.Scroll();
            ImGui::GetBackgroundDrawList()->AddRectFilled(MarkBehind.Min, MarkBehind.Max, IM_COL32_WHITE);
            // The window is held in its place, as the application may hold any of its windows.
            ImGui::SetNextWindowPos(depth > 0.0f ? ImVec2(100.0f, 20.0f) : ImVec2(0.0f, 0.0f), ImGuiCond_Always);
            ImGui::Begin(depth > 0.0f ? "tall" : "top", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove);
            if (depth > 0.0f) {
                ImGui::Dummy(ImVec2(200.0f, depth));
            }
            if (frames++ == 0) {
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::InputText("##field", text, sizeof text);
            field = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            ImGui::SameLine();
            if (ImGui::Button("count")) {
                clicks++;
            }
            button = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            ImGui::End();
            keyboard.Paint();
            ImGui::Render();
            const bool more = keyboard.Finish();
            ImGuiBridge::Describe(view, 0.0f);
            keyboard.Describe(view);
            const ImVec2 up(0.0f, -keyboard.GetLift());
            field.Translate(up);
            button.Translate(up);
            return more;
        }

        // Runs frames until the keyboard asks for no more: Dear ImGui takes a key's press and its
        // release in separate frames.
        void Settle() {
            for (int more = 0; more < 30 && Frame(); more++) {
            }
        }

        // Runs the frame the field takes focus in, and those the keyboard needs to come up.
        void Focus() {
            Frame();
            Settle();
        }

        ImVec2 At(std::string_view name) const {
            return KeyAt(keyboard.GetKeys(), name);
        }

        void Tap(std::string_view name) {
            const ImVec2 position = At(name);
            keyboard.OnTouch(0, Phase::Start, position, time);
            keyboard.OnTouch(0, Phase::End, position, time);
            Settle();
        }

        void Key(uint16_t code, bool pressed) {
            keyboard.OnKey(code, pressed, time);
            Settle();
        }

        void Press(uint16_t code) {
            keyboard.OnKey(code, true, time);
            keyboard.OnKey(code, false, time);
            Settle();
        }

        // A finger on the screen, sent where the router finds it belongs.
        TouchTarget Finger(Phase phase, ImVec2 position) {
            std::vector<InputEvent> events;
            const TouchTarget target = router.RouteContact(0, Contact{1, phase, 0.0f, 0.0f}, {position.x, position.y}, view, events);
            for (const InputEvent& event : events) {
                if (const auto* touch = std::get_if<InputEvent::KeyboardTouch>(&event.value)) {
                    keyboard.OnTouch(touch->id, touch->phase, ImVec2(touch->position[0], touch->position[1]), time);
                } else {
                    bridge.Feed(keyboard.ToInterface(event));
                }
            }
            Settle();
            return target;
        }

        TouchTarget Touch(ImVec2 position) {
            const TouchTarget target = Finger(Phase::Start, position);
            Finger(Phase::End, position);
            return target;
        }

        std::string_view Typed() const {
            return text;
        }
    };

    // What the stand-in for the system clipboard holds, for the test that shares it to look into.
    struct Outside {
        std::string text;
        int prepared = 0;
    };

    class Shared : public Clipboard {
    public:
        explicit Shared(Outside& outside) : outside_(outside) {
        }

        void Prepare() override {
            outside_.prepared++;
        }

        std::optional<std::string> Read() override {
            if (outside_.text.empty()) {
                return std::nullopt;
            }
            return outside_.text;
        }

        void Write(std::string_view text) override {
            outside_.text = text;
        }

    private:
        Outside& outside_;
    };
};

TEST(KeyboardManagerTest, "the on screen keyboard follows focus and types into the field") {
    Harness harness;
    const std::optional<Area> rect = harness.keyboard.GetRect();
    EXPECT(rect.has_value());
    if (!rect) {
        return;
    }
    // A field clear of the keys stays where it is.
    EXPECT(harness.field.Max.y < rect->top && harness.field.Min.y >= 0.0f);
    EXPECT_EQ(harness.keyboard.GetLift(), 0.0f);

    for (const char* key : {"shift", "O", "k", "symbols", "1", "backspace", "2"}) {
        harness.Tap(key);
    }
    EXPECT_EQ(harness.Typed(), "Ok2");
    harness.Tap("enter");
    EXPECT_EQ(ImGui::GetActiveID(), ImGuiID{0});
    EXPECT(!harness.keyboard.GetRect());
}

TEST(KeyboardManagerTest, "a hardware keyboard types and puts the on screen one away") {
    Harness harness;
    EXPECT(harness.keyboard.GetRect().has_value());

    harness.Press(35);
    harness.Press(23);
    EXPECT_EQ(harness.Typed(), "hi");
    EXPECT(!harness.keyboard.GetRect());
    EXPECT_EQ(ImGui::GetMainViewport()->WorkSize.y, 600.0f);
}

TEST(KeyboardManagerTest, "a field the keyboard would cover is lifted into view and the keyboard stays") {
    Harness harness(450.0f);
    size_t settled = 0;
    std::vector<bool> shown;
    for (size_t frame = 0; frame < 40; frame++) {
        if (harness.Frame()) {
            settled = frame + 1;
        }
        shown.push_back(harness.keyboard.GetRect().has_value());
    }
    const auto appeared = static_cast<size_t>(std::ranges::find(shown, true) - shown.begin());
    EXPECT(appeared < shown.size());
    if (appeared == shown.size()) {
        return;
    }
    // The keyboard does not come and go.
    EXPECT(std::ranges::count(shown, true) == static_cast<std::ptrdiff_t>(shown.size() - appeared));
    // Nor are frames asked for once it is up and the interface out of its way.
    EXPECT(settled <= appeared + 2);

    const Area keys = harness.keyboard.GetRect().value_or(Area{});
    EXPECT(harness.field.Max.y <= keys.top);
    EXPECT(harness.field.Min.y >= 0.0f);
    harness.Tap("x");
    EXPECT_EQ(harness.Typed(), "x");

    harness.Tap("enter");
    harness.Frame();
    harness.Frame();
    EXPECT(!harness.keyboard.GetRect());
    // The window is back where it was.
    EXPECT(harness.field.Min.y > keys.top);
    EXPECT_EQ(harness.keyboard.GetLift(), 0.0f);
}

TEST(KeyboardManagerTest, "the editing page selects cuts and pastes through the shared clipboard") {
    Outside system;
    Harness harness;
    harness.keyboard.ShareClipboard(std::make_unique<Shared>(system));
    system.text = "from outside";

    for (const char* key : {"symbols", "more"}) {
        harness.Tap(key);
    }
    // Nothing asks for the clipboard before its keys are in sight.
    EXPECT_EQ(system.prepared, 0);
    harness.Tap("more");
    EXPECT(system.prepared > 0);

    harness.Tap("paste");
    EXPECT_EQ(harness.Typed(), "from outside");
    for (const char* key : {"home", "right", "delete", "end", "left", "backspace"}) {
        harness.Tap(key);
    }
    EXPECT_EQ(harness.Typed(), "fom outsie");

    harness.Tap("all");
    harness.Tap("cut");
    EXPECT_EQ(harness.Typed(), "");
    EXPECT_EQ(system.text, "fom outsie");
    harness.Tap("undo");
    EXPECT_EQ(harness.Typed(), "fom outsie");
    harness.Tap("undo");
    harness.Tap("redo");
    EXPECT_EQ(harness.Typed(), "fom outsie");

    system.text.clear();
    // What was copied here stays when the system has no text.
    EXPECT_EQ(harness.keyboard.GetClipboard(), "fom outsie");
    harness.keyboard.SetClipboard("set from code");
    EXPECT_EQ(system.text, "set from code");
}

TEST(KeyboardManagerTest, "a hardware paste takes the shared clipboard and a copy goes to it") {
    Outside system;
    Harness harness;
    harness.keyboard.ShareClipboard(std::make_unique<Shared>(system));
    system.text = "one";

    const std::pair<uint16_t, bool> keys[] = {{29, true}, {47, true}, {47, false}, {30, true}, {30, false}, {46, true}, {46, false}, {29, false}};
    for (const auto& [code, pressed] : keys) {
        if (code == 30 && pressed) {
            system.text = "changed elsewhere";
        }
        harness.Key(code, pressed);
    }
    EXPECT_EQ(harness.Typed(), "one");
    // Copying the selection replaces what was there.
    EXPECT_EQ(system.text, "one");
    EXPECT(system.prepared > 0);
}

TEST(KeyboardManagerTest, "copied text comes back on paste") {
    Harness harness;
    harness.Press(30);
    harness.Press(48);

    const std::pair<uint16_t, bool> keys[] = {{29, true}, {30, true}, {30, false}, {46, true}, {46, false}, {47, true}, {47, false}, {47, true}, {47, false}, {29, false}};
    for (const auto& [code, pressed] : keys) {
        harness.Key(code, pressed);
    }
    EXPECT_EQ(harness.keyboard.GetClipboard(), "ab");
    EXPECT_EQ(harness.Typed(), "abab");
}

TEST(KeyboardManagerTest, "the keys are painted over everything inside the keyboard") {
    Harness harness;
    const Area rect = harness.keyboard.GetRect().value_or(Area{});
    const ImDrawData* frame = ImGui::GetDrawData();
    EXPECT(frame->CmdLists.Size > 0);
    if (frame->CmdLists.Size == 0) {
        return;
    }
    const ImDrawList* front = frame->CmdLists.back();
    EXPECT(front == ImGui::GetForegroundDrawList());
    // A key and its letter each take vertices; there are thirty-four keys.
    EXPECT(front->VtxBuffer.Size > 34 * 8);
    const ImRect painted = Bounds(*front);
    EXPECT(painted.Min.x >= rect.left - 1.0f && painted.Min.y >= rect.top - 1.0f && painted.Max.x <= rect.right + 1.0f && painted.Max.y <= rect.bottom + 1.0f);

    harness.keyboard.SetOnScreen(false);
    harness.Settle();
    EXPECT(!harness.keyboard.GetRect());
    const ImDrawData* bare = ImGui::GetDrawData();
    EXPECT(bare->CmdLists.Size > 0 && bare->CmdLists.back() != ImGui::GetForegroundDrawList());
}

TEST(KeyboardManagerTest, "the windows are drawn higher and what is behind and in front of them stays") {
    Harness harness(450.0f);
    harness.Focus();
    const float lift = harness.keyboard.GetLift();
    const Area keys = harness.keyboard.GetRect().value_or(Area{});
    EXPECT(lift > 100.0f);

    // Dear ImGui has the window where the application holds it.
    const ImGuiWindow* window = ImGui::FindWindowByName("tall");
    EXPECT_EQ(window->Pos.y, 20.0f);
    const ImRect drawn = Bounds(*window->DrawList);
    EXPECT_NEAR(drawn.Min.y, 20.0f - lift, 1.5);
    EXPECT_NEAR(drawn.Max.y, window->Pos.y + window->Size.y - lift, 1.5);
    for (const ImDrawCmd& command : window->DrawList->CmdBuffer) {
        EXPECT(command.ClipRect.w <= DisplaySize.y - lift);
    }

    const ImRect behind = Bounds(*ImGui::GetBackgroundDrawList());
    EXPECT_NEAR(behind.Min.y, MarkBehind.Min.y, 1.0);
    EXPECT_NEAR(behind.Max.y, MarkBehind.Max.y, 1.0);
    const ImRect front = Bounds(*ImGui::GetForegroundDrawList());
    EXPECT(front.Min.y >= keys.top - 1.0f && front.Max.y <= keys.bottom + 1.0f);
}

TEST(KeyboardManagerTest, "a finger finds the lifted interface where it is drawn and it stays under the finger") {
    Harness harness(450.0f);
    harness.Focus();
    const float lift = harness.keyboard.GetLift();
    const Area keys = harness.keyboard.GetRect().value_or(Area{});
    EXPECT(lift > 100.0f);

    // The router is told where the keys are, and where the window is on the screen.
    EXPECT(harness.view.keyboard == harness.keyboard.GetRect());
    EXPECT_EQ(harness.view.regions.size(), size_t{1});
    const ImGuiWindow* window = ImGui::FindWindowByName("tall");
    if (harness.view.regions.size() == 1) {
        EXPECT_EQ(harness.view.regions[0].bottom, window->Pos.y + window->Size.y - lift);
    }
    const ImVec2 button = harness.button.GetCenter();
    EXPECT(button.y < keys.top);

    EXPECT(harness.Finger(Phase::Start, button) == TouchTarget::Interface);
    // The press took the focus off the field and the keys went. Lowered now, the button
    // would be gone from under the finger before it lifts, and never be clicked.
    EXPECT(!harness.keyboard.GetRect());
    EXPECT_EQ(harness.keyboard.GetLift(), lift);
    harness.Finger(Phase::End, button);
    EXPECT_EQ(harness.clicks, 1);
    EXPECT_EQ(harness.keyboard.GetLift(), 0.0f);
    EXPECT(harness.field.Min.y > keys.top);

    // A finger that chooses the field holds the interface down as well, until it lifts.
    const ImVec2 field = harness.field.GetCenter();
    EXPECT(harness.Finger(Phase::Start, field) == TouchTarget::Interface);
    EXPECT(harness.keyboard.GetRect().has_value());
    EXPECT_EQ(harness.keyboard.GetLift(), 0.0f);
    harness.Finger(Phase::End, field);
    EXPECT_EQ(harness.keyboard.GetLift(), lift);
    EXPECT(harness.field.Max.y <= keys.top);

    // The keys take the touches that land on them, whatever Dear ImGui has underneath.
    EXPECT(harness.Touch(harness.At("q")) == TouchTarget::Keyboard);
    EXPECT_EQ(harness.Typed(), "q");
}

TEST(KeyboardManagerTest, "a tap elsewhere puts the keyboard away and a tap on the field brings it back") {
    Harness harness;
    const ImVec2 elsewhere(600.0f, 200.0f);
    EXPECT(harness.Touch(elsewhere) == TouchTarget::Outside);
    EXPECT_EQ(ImGui::GetActiveID(), ImGuiID{0});
    EXPECT(!harness.keyboard.GetRect());

    // A hardware key puts the on-screen keyboard away only for as long as the field keeps the focus.
    harness.Touch(harness.field.GetCenter());
    EXPECT(harness.keyboard.GetRect().has_value());
    harness.Press(44);
    EXPECT(!harness.keyboard.GetRect());
    harness.Touch(elsewhere);
    harness.Touch(harness.field.GetCenter());
    EXPECT(harness.keyboard.GetRect().has_value());
    harness.Tap("y");
    EXPECT_EQ(harness.Typed(), "zy");
}

TEST(KeyboardManagerTest, "a held hardware key goes on typing and a held backspace on deleting") {
    Harness harness;
    harness.keyboard.OnKey(30, true, harness.time);
    for (int frame = 0; frame < 120; frame++) {
        harness.Frame();
    }
    harness.keyboard.OnKey(30, false, harness.time);
    harness.Settle();
    // One for the press, then one every REPEAT_INTERVAL or so once REPEAT_DELAY has passed.
    const size_t typed = harness.Typed().size();
    EXPECT(typed >= 22 && typed <= 36);
    for (int frame = 0; frame < 30; frame++) {
        harness.Frame();
    }
    EXPECT_EQ(harness.Typed().size(), typed);

    // Dear ImGui repeats the key itself, at the same pace.
    harness.keyboard.OnKey(14, true, harness.time);
    for (int frame = 0; frame < 60; frame++) {
        harness.Frame();
    }
    harness.keyboard.OnKey(14, false, harness.time);
    harness.Settle();
    const size_t left = harness.Typed().size();
    EXPECT(typed - left >= 11 && typed - left <= 15);
    for (int frame = 0; frame < 30; frame++) {
        harness.Frame();
    }
    EXPECT_EQ(harness.Typed().size(), left);
}

TEST(KeyboardManagerTest, "a held on screen backspace goes on deleting and stops with the finger") {
    for (const double step : {1.0 / 60.0, 1.0 / 10.0}) {
        Harness harness;
        for (int letter = 0; letter < 40; letter++) {
            harness.Tap("a");
        }
        EXPECT_EQ(harness.Typed().size(), size_t{40});

        harness.step = step;
        const ImVec2 backspace = harness.At("backspace");
        harness.keyboard.OnTouch(7, Phase::Start, backspace, harness.time);
        const double until = harness.time + 1.0;
        while (harness.time < until) {
            harness.Frame();
        }
        harness.keyboard.OnTouch(7, Phase::End, backspace, harness.time);
        harness.Frame();
        const size_t left = harness.Typed().size();
        EXPECT(left >= 25 && left <= 36);
        // However slow the frames are, nothing is left over to go on deleting.
        for (int frame = 0; frame < 10; frame++) {
            harness.Frame();
        }
        EXPECT_EQ(harness.Typed().size(), left);
    }
}
