#include "Graphics/Texture.h"

#include "imgui_internal.h"

#include <cstring>
#include <mutex>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
#include "stb_image.h"

struct Texture::Retired {
    std::mutex mutex;
    std::vector<ImTextureData*> textures;
};

Texture::Retired& Texture::GetRetired() {
    // Never taken down: a texture may be let go of while the program is ending.
    static Retired& list = *new Retired;
    return list;
}

ImTextureRef Texture::GetRef() const {
    return data_ ? data_->GetTexRef() : ImTextureRef();
}

ImVec2 Texture::GetSize() const {
    return data_ ? ImVec2(static_cast<float>(data_->Width), static_cast<float>(data_->Height)) : ImVec2(0.0f, 0.0f);
}

uint32_t Texture::GetWidth() const {
    return data_ ? static_cast<uint32_t>(data_->Width) : 0;
}

uint32_t Texture::GetHeight() const {
    return data_ ? static_cast<uint32_t>(data_->Height) : 0;
}

Result<Texture::Picture> Texture::Decode(std::string_view name, std::span<const uint8_t> data) {
    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &width, &height, &channels, 4);
    if (!pixels || width <= 0 || height <= 0) {
        stbi_image_free(pixels);
        return Err("cannot read the picture {}: it is neither a PNG nor a JPEG this reader takes", name);
    }
    Picture picture{{static_cast<size_t>(width), static_cast<size_t>(height)}, {}};
    picture.rgba.assign(pixels, pixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    stbi_image_free(pixels);
    return picture;
}

Result<Texture> Texture::Load(std::string_view name, std::array<size_t, 2> size, std::span<const uint8_t> rgba, size_t limit) {
    const auto [width, height] = size;
    if (width == 0 || height == 0 || rgba.size() != width * height * 4) {
        return Err("the picture {} is said to be {}x{} and has {} bytes", name, width, height, rgba.size());
    }
    if (width > limit || height > limit) {
        return Err("the picture {} is {}x{}, and a texture here may be {} pixels a side", name, width, height, limit);
    }
    if (!ImGui::GetCurrentContext()) {
        return Err("the picture {} needs an overlay to be loaded into", name);
    }

    ImTextureData* data = IM_NEW(ImTextureData)();
    data->Create(ImTextureFormat_RGBA32, static_cast<int>(width), static_cast<int>(height));
    std::memcpy(data->GetPixels(), rgba.data(), rgba.size());
    data->UseColors = true;
    ImGui::RegisterUserTexture(data);
    const auto retire = [](ImTextureData* texture) {
        const std::scoped_lock lock(GetRetired().mutex);
        GetRetired().textures.push_back(texture);
    };
    return Texture(std::shared_ptr<ImTextureData>(data, retire));
}

bool Texture::Collect() {
    const std::scoped_lock lock(GetRetired().mutex);
    std::vector<ImTextureData*>& waiting = GetRetired().textures;
    const bool context = ImGui::GetCurrentContext() != nullptr;
    std::erase_if(waiting, [context](ImTextureData* texture) {
        const bool onRenderer = texture->GetTexID() != ImTextureID_Invalid && texture->Status != ImTextureStatus_Destroyed;
        if (context && onRenderer) {
            // Asked once; the renderer answers on the next frame it draws.
            texture->WantDestroyNextFrame = true;
            texture->UnusedFrames = 1;
            texture->Status = ImTextureStatus_WantDestroy;
            return false;
        }
        if (context) {
            ImGui::UnregisterUserTexture(texture);
        }
        IM_DELETE(texture);
        return true;
    });
    return !waiting.empty();
}
