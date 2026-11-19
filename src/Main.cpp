#include "Graphics/Graphics.h"
#include "Input/Touch/Touch.h"
#include "Window/ANativeWindowCreator.h"

#include "imgui.h"

#include <cfloat>
#include <chrono>
#include <cmath>
#include <format>
#include <memory>
#include <string>
#include <thread>
#include <utility>

int main(int argc, char** argv) {
    const GraphicsApi api = argc > 1 ? GraphicsManager::ParseApi(argv[1]).value_or(GraphicsApi::Auto) : GraphicsApi::Auto;

    NativeWindow window = ANativeWindowCreator::Create("android-imgui");
    const std::unique_ptr<Graphics> graphics = GraphicsManager::Create(window, api);
    if (!graphics || !touch.Init(*graphics)) {
        return 1;
    }

    const std::string title = std::format("Android ImGui · {}", graphics->GetName());
    float scale = 1.0f;
    Capture capture = Capture::Visible;
    bool mirroring = false;
    bool items = true;
    bool control = false;
    char text[128] = "";
    // A finger of its own, dragged across the screen while the real ones stay free. The thread
    // is waited for before the program ends.
    std::jthread swipe;

    while (graphics->NewFrame()) {
        const ImVec2 screen = ImGui::GetIO().DisplaySize;

        // Things that are not part of the menu: boxes that follow positions of their own, drawn
        // behind the windows in screen pixels. Nothing drawn there takes a touch.
        if (items) {
            ImDrawList* behind = ImGui::GetBackgroundDrawList();
            const float time = static_cast<float>(ImGui::GetTime());
            for (int index = 0; index < 5; index++) {
                const float turn = time * (0.3f + static_cast<float>(index) * 0.07f) + static_cast<float>(index) * 1.3f;
                const float x = screen.x * (0.5f + 0.38f * std::cos(turn)), y = screen.y * (0.5f + 0.3f * std::sin(turn * 1.4f));
                behind->AddLine({screen.x / 2.0f, screen.y}, {x, y + 90.0f}, IM_COL32(255, 255, 255, 90), 2.0f);
                behind->AddRect({x - 45.0f, y - 90.0f}, {x + 45.0f, y + 90.0f}, IM_COL32(80, 220, 120, 255), 0.0f, ImDrawFlags_None, 3.0f);
                const std::string label = std::format("item {}", index);
                const ImVec2 extent = ImGui::GetFont()->CalcTextSizeA(30.0f, FLT_MAX, 0.0f, label.c_str());
                behind->AddText(ImGui::GetFont(), 30.0f, {x - extent.x / 2.0f, y - 98.0f - extent.y}, IM_COL32_WHITE, label.c_str());
            }
            graphics->RequestRepaint();
        }

        ImGui::SetNextWindowPos(ImVec2(screen.x / 2.0f, screen.y / 2.0f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        if (ImGui::Begin(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::SliderFloat("Scale", &scale, 0.5f, 3.0f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                graphics->SetScale(scale);
            }
            int picked = std::to_underlying(capture);
            if (ImGui::Combo("Screenshots and recordings", &picked, "Visible\0Hidden\0Secure\0")) {
                capture = static_cast<Capture>(picked);
            }
            if (capture == Capture::Visible) {
                ImGui::Checkbox("Mirror to screen recorders", &mirroring);
            }
            ImGui::Checkbox("Draw items", &items);
            ImGui::Checkbox("Control touches", &control);
            if (control && ImGui::Button("Swipe up")) {
                swipe = std::jthread([screen] { touch.Swipe({screen.x * 0.75f, screen.y * 0.7f}, {screen.x * 0.75f, screen.y * 0.3f}, std::chrono::milliseconds(400)); });
            }
            ImGui::InputText("##text", text, sizeof text);
            if (ImGui::Button("Close")) {
                graphics->Close();
            }
        }
        ImGui::End();
        graphics->EndFrame();

        if (!window.SetCapture(capture)) {
            capture = window.GetCapture();
        }
        window.SetMirroring(mirroring);
        if (control != touch.IsControl()) {
            control = touch.SetControl(control) && control;
        }
    }
    return 0;
}
