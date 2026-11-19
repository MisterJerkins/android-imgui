#pragma once

#include "Graphics/Graphics.h"
#include "Graphics/OpenGL/Egl.h"

#include <cstdint>
#include <memory>
#include <vector>

struct ImTextureData;

// Draws with OpenGL ES, using only what ES 2 has, so one renderer serves ES 2 and ES 3.
class OpenGLGraphics final : public Graphics {
public:
    // The graphics on `window`, ready for frames.
    static Result<std::unique_ptr<Graphics>> Create(const NativeWindow& window);
    ~OpenGLGraphics() override;

    const char* GetName() const override {
        return "OpenGL ES";
    }

private:
    OpenGLGraphics(const NativeWindow& window, Egl egl) : Graphics(window), egl_(std::move(egl)) {
    }

    size_t GetMaxTextureSide() const override {
        return maxTextureSide_;
    }
    Result<> Rebind(const NativeWindow& window) override;
    Result<> Render(const NativeWindow& window, ImDrawData& frame) override;
    static Result<uint32_t> Compile(uint32_t kind, const char* source);
    static uint32_t NameOf(const ImTextureData& texture);
    Result<> Start();
    void UpdateTexture(ImTextureData& texture);

    Egl egl_;
    uint32_t program_ = 0;
    uint32_t vertices_ = 0;
    uint32_t indices_ = 0;
    int32_t projection_ = -1;
    int32_t sampler_ = -1;
    int32_t position_ = -1;
    int32_t uv_ = -1;
    int32_t color_ = -1;
    size_t maxTextureSide_ = 2048;
    std::vector<uint8_t> scratch_;
};
