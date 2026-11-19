#pragma once

#include "Utils/Utils.h"

#include "imgui.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

// A picture loaded for drawing: `ImGui::Image(texture.GetRef(), texture.GetSize())` puts it into
// a window, `ImGui::GetBackgroundDrawList()->AddImage(texture.GetRef(), ...)` anywhere on the
// screen. The graphics keep it until the last copy of the handle is gone.
class Texture {
public:
    // A picture as its size and RGBA pixels, four bytes each, row after row from the top.
    struct Picture {
        std::array<size_t, 2> size{};
        std::vector<uint8_t> rgba;
    };

    Texture() = default;

    // Reads a PNG or JPEG picture. `name` tells it from others in messages.
    static Result<Picture> Decode(std::string_view name, std::span<const uint8_t> data);
    // Hands pixels to the graphics. `limit` is the longest side a texture may have.
    static Result<Texture> Load(std::string_view name, std::array<size_t, 2> size, std::span<const uint8_t> rgba, size_t limit);
    // Lets go of the textures whose last handle is gone, once the renderer has destroyed its
    // side of them. Called by the graphics after each frame; `true` while some are still waiting.
    static bool Collect();

    // What Dear ImGui's image calls take.
    ImTextureRef GetRef() const;
    // The picture's size in pixels.
    ImVec2 GetSize() const;
    uint32_t GetWidth() const;
    uint32_t GetHeight() const;

    explicit operator bool() const {
        return static_cast<bool>(data_);
    }

private:
    // Textures nobody holds any more. The renderer's side of each is destroyed on the frames'
    // thread, whichever thread dropped the last handle.
    struct Retired;

    explicit Texture(std::shared_ptr<ImTextureData> data) : data_(std::move(data)) {
    }
    static Retired& GetRetired();

    std::shared_ptr<ImTextureData> data_;
};
