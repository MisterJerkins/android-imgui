#include "Graphics/Blur.h"

#include "Test.h"

#include "imgui_internal.h"

#include <format>
#include <span>
#include <string>
#include <vector>

class BlurTest {
protected:
    // Dear ImGui without a screen, a thousand pixels each way, and the parts to blur in one
    // frame of what `ui` builds.
    struct Harness {
        Blur blur{true};

        Harness() {
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
            io.DisplaySize = ImVec2(1000.0f, 1000.0f);
        }
        ~Harness() {
            ImGui::DestroyContext();
        }

        template <class Ui>
        std::vector<std::string> Frame(Ui&& ui, float lift = 0.0f) {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            blur.NewFrame();
            ImGui::NewFrame();
            ui();
            ImGui::Render();
            std::vector<std::string> parts;
            for (const BlurRegion& region : blur.Collect(*ImGui::GetCurrentContext(), lift)) {
                parts.push_back(std::format("{} px at {},{} to {},{} corners {} {} {} {}", region.radius, region.left, region.top, region.right, region.bottom, region.cornerTopLeft, region.cornerTopRight, region.cornerBottomLeft, region.cornerBottomRight));
            }
            return parts;
        }

        // A window that is where it is told to be from its first frame on.
        static bool Begin(const char* name, ImVec2 at, ImVec2 size, ImGuiWindowFlags flags = ImGuiWindowFlags_None) {
            ImGui::SetNextWindowPos(at, ImGuiCond_Always);
            ImGui::SetNextWindowSize(size, ImGuiCond_Always);
            return ImGui::Begin(name, nullptr, flags | ImGuiWindowFlags_NoSavedSettings);
        }
    };

    using Parts = std::vector<std::string>;
};

TEST(BlurTest, "nothing is blurred until a radius is given") {
    Harness harness;
    const auto window = [] {
        Harness::Begin("menu", {100.0f, 200.0f}, {300.0f, 400.0f});
        ImGui::End();
    };
    EXPECT(harness.Frame(window).empty());
    EXPECT_EQ(harness.blur.GetWindowRadius(), 0.0f);
}

TEST(BlurTest, "a window is blurred over its whole rectangle, with its rounding") {
    Harness harness;
    harness.blur.SetWindowRadius(40.0f);
    const auto window = [] {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
        Harness::Begin("menu", {100.0f, 200.0f}, {300.0f, 400.0f});
        ImGui::End();
        ImGui::PopStyleVar();
    };
    EXPECT(harness.Frame(window) == Parts{"40 px at 100,200 to 400,600 corners 12 12 12 12"});
}

TEST(BlurTest, "a window without a background and a window inside another are left out") {
    Harness harness;
    harness.blur.SetWindowRadius(40.0f);
    const auto windows = [] {
        Harness::Begin("bare", {10.0f, 10.0f}, {50.0f, 50.0f}, ImGuiWindowFlags_NoBackground);
        ImGui::End();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        Harness::Begin("menu", {100.0f, 200.0f}, {300.0f, 400.0f});
        ImGui::BeginChild("inside", {100.0f, 100.0f});
        ImGui::EndChild();
        ImGui::End();
        ImGui::PopStyleVar();
    };
    EXPECT(harness.Frame(windows) == Parts{"40 px at 100,200 to 400,600 corners 0 0 0 0"});
}

TEST(BlurTest, "a rectangle is blurred where a filled one would be drawn") {
    Harness harness;
    const auto rectangle = [&] { harness.blur.AddRect(ImGui::GetBackgroundDrawList(), {10.2f, 20.7f}, {110.2f, 220.7f}, 25.0f, 8.0f); };
    EXPECT(harness.Frame(rectangle) == Parts{"25 px at 10,21 to 110,221 corners 8 8 8 8"});
}

TEST(BlurTest, "a rectangle is cut as the drawing is, and a cut corner is square") {
    Harness harness;
    const auto rectangle = [&] {
        ImDrawList* list = ImGui::GetBackgroundDrawList();
        list->PushClipRect({200.0f, 200.0f}, {300.0f, 300.0f});
        harness.blur.AddRect(list, {150.0f, 150.0f}, {250.0f, 250.0f}, 30.0f, 10.0f);
        list->PopClipRect();
    };
    EXPECT(harness.Frame(rectangle) == Parts{"30 px at 200,200 to 250,250 corners 0 0 0 10"});
}

TEST(BlurTest, "what is in the windows goes up above a keyboard with them, what is behind them stays") {
    Harness harness;
    harness.blur.SetWindowRadius(40.0f);
    const auto scene = [&] {
        harness.blur.AddRect(ImGui::GetBackgroundDrawList(), {0.0f, 500.0f}, {100.0f, 600.0f}, 40.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        Harness::Begin("menu", {100.0f, 200.0f}, {300.0f, 400.0f});
        harness.blur.AddRect(ImGui::GetWindowDrawList(), {150.0f, 300.0f}, {250.0f, 350.0f}, 40.0f);
        ImGui::End();
        ImGui::PopStyleVar();
    };
    const Parts lifted = harness.Frame(scene, 150.0f);
    EXPECT(lifted == (Parts{"40 px at 100,50 to 400,450 corners 0 0 0 0", "40 px at 0,500 to 100,600 corners 0 0 0 0", "40 px at 150,150 to 250,200 corners 0 0 0 0"}));
}

TEST(BlurTest, "what is off the screen or has no radius is not asked for") {
    Harness harness;
    const auto rectangles = [&] {
        ImDrawList* list = ImGui::GetBackgroundDrawList();
        harness.blur.AddRect(list, {-300.0f, 100.0f}, {-100.0f, 200.0f}, 30.0f);
        harness.blur.AddRect(list, {100.0f, 100.0f}, {200.0f, 200.0f}, 0.0f);
        harness.blur.AddRect(list, {900.0f, 900.0f}, {1200.0f, 1200.0f}, 30.0f);
    };
    EXPECT(harness.Frame(rectangles) == Parts{"30 px at 900,900 to 1000,1000 corners 0 0 0 0"});
}

TEST(BlurTest, "a fourth radius in a frame takes the nearest of the three in use") {
    Harness harness;
    const auto rectangles = [&] {
        ImDrawList* list = ImGui::GetBackgroundDrawList();
        for (const float radius : {10.0f, 20.0f, 30.0f, 27.0f, 20.0f, 100.0f}) {
            harness.blur.AddRect(list, {0.0f, 0.0f}, {50.0f, 50.0f}, radius);
        }
    };
    const Parts parts = harness.Frame(rectangles);
    EXPECT_EQ(parts.size(), size_t{6});
    EXPECT(parts[3].starts_with("30 px") && parts[4].starts_with("20 px") && parts[5].starts_with("30 px"));
}

TEST(BlurTest, "the rectangles of a frame are gone in the next") {
    Harness harness;
    EXPECT_EQ(harness.Frame([&] { harness.blur.AddRect(ImGui::GetBackgroundDrawList(), {0.0f, 0.0f}, {50.0f, 50.0f}, 30.0f); }).size(), size_t{1});
    EXPECT(harness.Frame([] {}).empty());
}

TEST(BlurTest, "where the system cannot blur nothing is asked of it") {
    Harness harness;
    harness.blur = Blur(false);
    harness.blur.SetWindowRadius(40.0f);
    const auto scene = [&] {
        harness.blur.AddRect(ImGui::GetBackgroundDrawList(), {0.0f, 0.0f}, {50.0f, 50.0f}, 30.0f);
        Harness::Begin("menu", {100.0f, 200.0f}, {300.0f, 400.0f});
        ImGui::End();
    };
    EXPECT(!harness.blur.IsAvailable());
    EXPECT(harness.Frame(scene).empty());
}
