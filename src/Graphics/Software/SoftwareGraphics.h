#pragma once

#include "Graphics/Graphics.h"
#include "Graphics/Software/Rasterizer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// Draws on the CPU. The picture lives in memory of our own and only the tiles a frame
// changes are drawn again; each of the window's buffers is then brought up to date with
// just the part it has missed. Asking the system to carry the rest over instead would
// have it copy nearly the whole screen on every frame.
class SoftwareGraphics final : public Graphics {
public:
    // The graphics on `window`, ready for frames.
    static Result<std::unique_ptr<Graphics>> Create(const NativeWindow& window);
    ~SoftwareGraphics() override;

    const char* GetName() const override {
        return "software";
    }

private:
    explicit SoftwareGraphics(const NativeWindow& window);

    size_t GetMaxTextureSide() const override {
        return 8192;
    }
    Result<> Rebind(const NativeWindow& window) override;
    Result<> Render(const NativeWindow& window, ImDrawData& frame) override;
    Result<> Present(const NativeWindow& window, Region screen, Region damage);

    SoftwareTextures textures_;
    Damage damage_;
    std::vector<uint32_t> picture_;
    std::array<size_t, 2> size_{};
    BufferHistory behind_;
    size_t workers_ = 1;
};
