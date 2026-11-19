#include "Input/Touch/ImGuiBridge.h"

#include "Input/Touch/TouchRouter.h"
#include "Test.h"

#include "imgui_internal.h"

#include <cmath>
#include <vector>

class ImGuiBridgeTest {
protected:
    // Dear ImGui without a screen, a window with a button, a slider and a long list in it, and
    // touches routed into it the way the overlay routes them.
    struct Harness {
        TouchRouter router;
        ImGuiBridge bridge;
        InterfaceView view;
        float offset = 0.0f;
        float level = 0.5f;
        int clicks = 0;
        int rowClicks = 0;
        float left = 0.0f;
        float top = 0.0f;
        bool gliding = false;

        Harness() {
            view.size = {1000.0f, 1000.0f};
            view.regions = {{0.0f, 0.0f, 1000.0f, 1000.0f}};
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
            io.ConfigWindowsMoveFromTitleBarOnly = true;
            io.DisplaySize = ImVec2(1000.0f, 1000.0f);
            Frame({});
            Frame({});
        }
        ~Harness() {
            ImGui::DestroyContext();
        }

        void Frame(const std::vector<InputEvent>& events, float seconds = 1.0f / 60.0f) {
            for (const InputEvent& event : events) {
                bridge.Feed(event);
            }
            ImGui::GetIO().DeltaTime = seconds;
            ImGui::NewFrame();
            ImGuiBridge::Confine();
            gliding = bridge.Scroll();
            ImGui::SetNextWindowPos(ImVec2(100.0f, 100.0f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(400.0f, 600.0f), ImGuiCond_FirstUseEver);
            ImGui::Begin("list");
            left = ImGui::GetWindowPos().x;
            top = ImGui::GetWindowPos().y;
            if (ImGui::Button("tap", ImVec2(300.0f, 60.0f))) {
                clicks++;
            }
            ImGui::SetNextItemWidth(300.0f);
            ImGui::SliderFloat("level", &level, 0.0f, 1.0f);
            ImGui::BeginChild("rows", ImVec2(0.0f, 300.0f));
            for (int row = 0; row < 100; row++) {
                if (row % 10 == 5) {
                    ImGui::PushID(row);
                    if (ImGui::Button("row button", ImVec2(300.0f, 0.0f))) {
                        rowClicks++;
                    }
                    ImGui::PopID();
                } else {
                    ImGui::Text("row %d", row);
                }
            }
            offset = ImGui::GetScrollY();
            ImGui::EndChild();
            ImGui::End();
            ImGui::Render();
        }

        std::vector<InputEvent> Route(Phase phase, std::array<float, 2> position) {
            std::vector<InputEvent> events;
            router.RouteContact(0, Contact{1, phase, 0.0f, 0.0f}, position, view, events);
            return events;
        }

        void Touch(Phase phase, std::array<float, 2> position) {
            Frame(Route(phase, position));
        }
    };
};

TEST(ImGuiBridgeTest, "a tap clicks the button") {
    Harness harness;
    harness.Touch(Phase::Start, {200.0f, 150.0f});
    harness.Touch(Phase::End, {200.0f, 150.0f});
    harness.Frame({});
    EXPECT_EQ(harness.clicks, 1);
}

// Frames that take over a second each, as with software drawing on a loaded emulator: a tap
// of fifty milliseconds is still a click.
TEST(ImGuiBridgeTest, "a tap clicks however slow the frames are") {
    Harness harness;
    harness.Frame(harness.Route(Phase::Start, {200.0f, 150.0f}), 1.2f);
    harness.Frame(harness.Route(Phase::End, {200.0f, 150.0f}), 1.2f);
    harness.Frame({}, 1.2f);
    EXPECT_EQ(harness.clicks, 1);
}

TEST(ImGuiBridgeTest, "a press and its release in one frame still click") {
    Harness harness;
    std::vector<InputEvent> events = harness.Route(Phase::Start, {200.0f, 150.0f});
    const std::vector<InputEvent> up = harness.Route(Phase::End, {200.0f, 150.0f});
    events.insert(events.end(), up.begin(), up.end());
    harness.Frame(events);
    for (int frames = 0; frames < 4; frames++) {
        harness.Frame({});
    }
    EXPECT_EQ(harness.clicks, 1);
}

// A whole drag queued before a single frame ran, as after a stall: the finger leaves the
// title bar at once and lifts 200 pixels to the right.
TEST(ImGuiBridgeTest, "a drag that outruns the frames still moves the window") {
    Harness harness;
    const float start = harness.left;
    EventQueue queue;
    std::vector<InputEvent> events;
    const std::pair<Phase, std::array<float, 2>> steps[] = {{Phase::Start, {240.0f, 108.0f}}, {Phase::Move, {330.0f, 160.0f}}, {Phase::Move, {440.0f, 160.0f}}, {Phase::End, {440.0f, 160.0f}}};
    for (const auto& [phase, position] : steps) {
        harness.router.RouteContact(0, Contact{1, phase, 0.0f, 0.0f}, position, harness.view, events);
    }
    const auto now = std::chrono::steady_clock::now();
    for (InputEvent& event : events) {
        queue.Push(std::move(event), now);
    }
    while (!queue.IsEmpty()) {
        std::vector<InputEvent> taken;
        queue.TakeFrame(taken);
        harness.Frame(taken);
    }
    for (int frames = 0; frames < 3; frames++) {
        harness.Frame({});
    }
    EXPECT_NEAR(harness.left - start, 200.0, 1.0);
}

TEST(ImGuiBridgeTest, "a finger drag scrolls the list and keeps gliding") {
    Harness harness;
    harness.Touch(Phase::Start, {200.0f, 420.0f});
    for (int step = 1; step <= 10; step++) {
        harness.Touch(Phase::Move, {200.0f, 420.0f - static_cast<float>(step) * 15.0f});
    }
    harness.Frame({});
    const float dragged = harness.offset;
    EXPECT(dragged > 100.0f);
    EXPECT_EQ(harness.left, 100.0f);

    harness.Touch(Phase::End, {200.0f, 270.0f});
    for (int frames = 0; frames < 20; frames++) {
        harness.Frame({});
    }
    EXPECT(harness.offset > dragged + 10.0f);
    EXPECT_EQ(harness.rowClicks, 0);

    for (int frames = 0; frames < 600 && harness.gliding; frames++) {
        harness.Frame({});
    }
    EXPECT(!harness.gliding);
}

TEST(ImGuiBridgeTest, "a new touch stops the glide") {
    Harness harness;
    harness.Touch(Phase::Start, {200.0f, 420.0f});
    for (int step = 1; step <= 10; step++) {
        harness.Touch(Phase::Move, {200.0f, 420.0f - static_cast<float>(step) * 15.0f});
    }
    harness.Touch(Phase::End, {200.0f, 270.0f});
    harness.Frame({});
    EXPECT(harness.gliding);
    harness.Touch(Phase::Start, {200.0f, 400.0f});
    harness.Frame({});
    const float held = harness.offset;
    for (int frames = 0; frames < 5; frames++) {
        harness.Frame({});
    }
    EXPECT_EQ(harness.offset, held);
}

TEST(ImGuiBridgeTest, "a slider keeps the drag that started on it") {
    Harness harness;
    harness.Touch(Phase::Start, {150.0f, 205.0f});
    for (int step = 1; step <= 8; step++) {
        harness.Touch(Phase::Move, {150.0f + static_cast<float>(step) * 20.0f, 205.0f + static_cast<float>(step) * 6.0f});
    }
    harness.Touch(Phase::End, {310.0f, 253.0f});
    harness.Frame({});
    EXPECT(harness.level > 0.6f);
    EXPECT_EQ(harness.offset, 0.0f);
}

TEST(ImGuiBridgeTest, "a finger that leaves without lifting clicks nothing") {
    Harness harness;
    harness.Touch(Phase::Start, {200.0f, 150.0f});
    harness.Touch(Phase::Cancel, {200.0f, 150.0f});
    for (int frames = 0; frames < 3; frames++) {
        harness.Frame({});
    }
    EXPECT_EQ(harness.clicks, 0);
    EXPECT(!ImGui::GetIO().MouseDown[0]);
}

TEST(ImGuiBridgeTest, "the view lists the windows that take touches") {
    Harness harness;
    InterfaceView view;
    ImGuiBridge::Describe(view, 8.0f);
    EXPECT_EQ(view.regions.size(), size_t{1});
    if (view.regions.size() == 1) {
        EXPECT(view.regions[0] == (Area{92.0f, 92.0f, 508.0f, 708.0f}));
    }
    EXPECT(!view.dismissable);
    EXPECT(!view.wantsKeys);
}

TEST(ImGuiBridgeTest, "a window dragged past a corner stops in it") {
    Harness harness;
    harness.Touch(Phase::Start, {240.0f, 108.0f});
    for (int step = 1; step <= 10; step++) {
        harness.Touch(Phase::Move, {240.0f - static_cast<float>(step) * 30.0f, 108.0f - static_cast<float>(step) * 30.0f});
    }
    harness.Frame({});
    EXPECT_EQ(harness.left, 0.0f);
    EXPECT_EQ(harness.top, 0.0f);
    harness.Touch(Phase::End, {-60.0f, -192.0f});
    for (int frames = 0; frames < 3; frames++) {
        harness.Frame({});
    }
    EXPECT_EQ(harness.left, 0.0f);
    EXPECT_EQ(harness.top, 0.0f);

    harness.Touch(Phase::Start, {140.0f, 8.0f});
    for (int step = 1; step <= 10; step++) {
        harness.Touch(Phase::Move, {140.0f + static_cast<float>(step) * 100.0f, 8.0f + static_cast<float>(step) * 100.0f});
    }
    harness.Touch(Phase::End, {1140.0f, 1008.0f});
    for (int frames = 0; frames < 3; frames++) {
        harness.Frame({});
    }
    EXPECT_EQ(harness.left, 600.0f);
    EXPECT_EQ(harness.top, 400.0f);
}
