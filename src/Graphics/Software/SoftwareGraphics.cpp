#include "Graphics/Software/SoftwareGraphics.h"

#include "Utils/Logger.h"
#include "Window/ANativeWindowCreator.h"

#include "imgui.h"

#include <algorithm>
#include <ranges>
#include <thread>
#include <utility>

Result<std::unique_ptr<Graphics>> SoftwareGraphics::Create(const NativeWindow& window) {
    std::unique_ptr<SoftwareGraphics> graphics(new SoftwareGraphics(window));
    graphics->Ready();
    return graphics;
}

// A redraw is shared between four threads at most: more of them only wait for memory.
SoftwareGraphics::SoftwareGraphics(const NativeWindow& window) : Graphics(window), workers_(std::clamp<size_t>(std::thread::hardware_concurrency(), 1, 4)) {
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "android-imgui software";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    platform.Renderer_TextureMaxWidth = platform.Renderer_TextureMaxHeight = static_cast<int>(GetMaxTextureSide());
    logger.Info("software backend ready, up to {} threads", workers_);
}

SoftwareGraphics::~SoftwareGraphics() {
    if (!ImGui::GetCurrentContext()) {
        return;
    }
    for (ImTextureData* texture : ImGui::GetPlatformIO().Textures) {
        if (texture->RefCount == 1 && textures_.Find(texture->GetTexID())) {
            textures_.Destroy(*texture);
        }
    }
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset);
    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    platform.Renderer_TextureMaxWidth = platform.Renderer_TextureMaxHeight = 0;
}

Result<> SoftwareGraphics::Rebind(const NativeWindow&) {
    damage_.Reset();
    behind_.Clear();
    return {};
}

Result<> SoftwareGraphics::Render(const NativeWindow& window, ImDrawData& frame) {
    const std::vector<ImTextureID> repainted = textures_.Update(frame);

    const int32_t width = static_cast<int32_t>(frame.DisplaySize.x * frame.FramebufferScale.x), height = static_cast<int32_t>(frame.DisplaySize.y * frame.FramebufferScale.y);
    if (width <= 0 || height <= 0) {
        return {};
    }
    const std::array size{static_cast<size_t>(width), static_cast<size_t>(height)};
    if (size_ != size) {
        size_ = size;
        picture_.assign(size[0] * size[1], 0);
        damage_.Reset();
        behind_.Clear();
    }

    const Region screen{0, 0, width, height};
    const std::vector<Mesh> drawn = Mesh::Collect(frame, screen);
    const Region damage = damage_.Update(screen, drawn, repainted);
    if (damage.IsEmpty()) {
        return {};
    }

    const auto draw = [&](Canvas canvas) {
        canvas.Clear();
        canvas.DrawMeshes(drawn, textures_);
    };
    const Canvas canvas(picture_, size[0], damage);
    // From this many pixels on, a redraw is shared between threads.
    if (damage.GetArea() < 200'000 || workers_ < 2) {
        draw(canvas);
    } else {
        // Threads made for the frame and gone with it: on the devices this was measured on they
        // cost less than threads kept waiting between frames, which the scheduler is slow to wake.
        const std::vector<Canvas> bands = canvas.Bands(workers_);
        std::vector<std::jthread> threads;
        for (const Canvas& band : bands | std::views::drop(1)) {
            threads.emplace_back(draw, band);
        }
        draw(bands.front());
        // Leaving here waits for every one of them.
    }
    return Present(window, screen, damage);
}

Result<> SoftwareGraphics::Present(const NativeWindow& window, Region screen, Region damage) {
    Result<WindowBuffer> locked = window.Lock();
    if (!locked) {
        // No buffer has what this frame changed, and nothing remembers that it is owed: the
        // next frame is drawn and handed over whole.
        damage_.Reset();
        behind_.Clear();
        return std::unexpected(std::move(locked.error()));
    }
    WindowBuffer& buffer = *locked;
    const Region bounds{0, 0, static_cast<int32_t>(buffer.GetWidth()), static_cast<int32_t>(buffer.GetHeight())};
    const Region missing = behind_.Missing(buffer.GetPixels().data(), screen, damage).Intersect(bounds);
    if (missing.IsEmpty()) {
        return {};
    }

    const size_t left = static_cast<size_t>(missing.left), count = static_cast<size_t>(missing.right - missing.left);
    for (size_t y = static_cast<size_t>(missing.top); y < static_cast<size_t>(missing.bottom); y++) {
        const std::span<uint32_t> row = buffer.GetRow(y);
        if (row.size() < left + count) {
            break;
        }
        std::copy_n(picture_.data() + y * size_[0] + left, count, row.data() + left);
    }
    return {};
}
