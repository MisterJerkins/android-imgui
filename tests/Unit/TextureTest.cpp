#include "Graphics/Texture.h"

#include "Pictures.h"
#include "Test.h"

#include "imgui_internal.h"

#include <cstdlib>
#include <span>
#include <vector>

class TextureTest {
protected:
    // A Dear ImGui context that draws nowhere, with one frame open for as long as it lives.
    struct Frame {
        Frame() {
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
            io.DisplaySize = ImVec2(800.0f, 600.0f);
            io.DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
        }
        ~Frame() {
            Texture::Collect();
            ImGui::DestroyContext();
        }
    };

    struct Bounds {
        float left = 1e9f, top = 1e9f, right = -1e9f, bottom = -1e9f;
    };

    static Bounds BoundsOf(std::span<const ImDrawVert> vertices) {
        Bounds box;
        for (const ImDrawVert& vertex : vertices) {
            box.left = std::min(box.left, vertex.pos.x);
            box.top = std::min(box.top, vertex.pos.y);
            box.right = std::max(box.right, vertex.pos.x);
            box.bottom = std::max(box.bottom, vertex.pos.y);
        }
        return box;
    }
};

TEST(TextureTest, "a png comes back pixel for pixel with its transparency") {
    const auto picture = Texture::Decode("test", PicturePng);
    EXPECT(picture.has_value());
    if (!picture) {
        return;
    }
    EXPECT_EQ(picture->size[0], size_t{16});
    EXPECT_EQ(picture->size[1], size_t{8});
    EXPECT_EQ(picture->rgba.size(), size_t{16 * 8 * 4});
    for (size_t index = 0; index < 16 * 8; index++) {
        const size_t x = index % 16, y = index / 16;
        const uint8_t wanted[4] = {static_cast<uint8_t>(x * 16), static_cast<uint8_t>(y * 30), 200, static_cast<uint8_t>(x < 8 ? 255 : 90)};
        for (size_t channel = 0; channel < 4; channel++) {
            EXPECT_EQ(picture->rgba[index * 4 + channel], wanted[channel]);
        }
    }
}

TEST(TextureTest, "a jpeg comes back close to its colours and opaque") {
    const auto picture = Texture::Decode("test", PictureJpeg);
    EXPECT(picture.has_value());
    if (!picture) {
        return;
    }
    EXPECT_EQ(picture->size[0], size_t{16});
    EXPECT_EQ(picture->size[1], size_t{8});
    const int wanted[4] = {200, 120, 40, 255};
    for (size_t index = 0; index < picture->rgba.size(); index++) {
        EXPECT(std::abs(picture->rgba[index] - wanted[index % 4]) <= 4);
    }
}

TEST(TextureTest, "what is not a picture is an error") {
    const unsigned char text[] = "not a picture";
    EXPECT(!Texture::Decode("test", text));
}

TEST(TextureTest, "pixels become a texture of their size") {
    const Frame frame;
    const std::vector<uint8_t> white(24, 255);
    const auto texture = Texture::Load("test", {3, 2}, white, 2048);
    EXPECT(texture.has_value());
    if (texture) {
        EXPECT_EQ(texture->GetWidth(), uint32_t{3});
        EXPECT_EQ(texture->GetHeight(), uint32_t{2});
    }
    EXPECT(!Texture::Load("short", {3, 2}, std::span(white).first(20), 2048));
    EXPECT(!Texture::Load("empty", {0, 2}, {}, 2048));
    EXPECT(!Texture::Load("huge", {3, 2}, white, 2));
}

TEST(TextureTest, "a picture needs an overlay to be loaded into") {
    const std::vector<uint8_t> white(24, 255);
    EXPECT(!Texture::Load("test", {3, 2}, white, 2048));
}

TEST(TextureTest, "a picture is drawn with its texture where it is put") {
    const Frame frame;
    const std::vector<uint8_t> white(16, 255);
    const auto picture = Texture::Load("test", {2, 2}, white, 2048);
    EXPECT(picture.has_value());
    if (!picture) {
        return;
    }
    ImGui::GetBackgroundDrawList()->AddImage(picture->GetRef(), {20.0f, 40.0f}, {60.0f, 80.0f});
    ImGui::Render();
    bool drawn = false;
    for (const ImDrawList* list : ImGui::GetDrawData()->CmdLists) {
        for (const ImDrawCmd& command : list->CmdBuffer) {
            if (command.TexRef._TexData != picture->GetRef()._TexData || command.ElemCount == 0) {
                continue;
            }
            const std::span<const ImDrawVert> vertices(list->VtxBuffer.Data + command.VtxOffset, 4);
            const Bounds box = BoundsOf(vertices);
            drawn = box.left == 20.0f && box.top == 40.0f && box.right == 60.0f && box.bottom == 80.0f;
        }
    }
    EXPECT(drawn);
}

TEST(TextureTest, "a picture nobody holds is let go") {
    const Frame frame;
    const std::vector<uint8_t> white(16, 255);
    const int before = ImGui::GetCurrentContext()->UserTextures.Size;
    {
        const auto picture = Texture::Load("test", {2, 2}, white, 2048);
        EXPECT(picture.has_value());
        EXPECT_EQ(ImGui::GetCurrentContext()->UserTextures.Size, before + 1);
        EXPECT(!Texture::Collect());
        EXPECT_EQ(ImGui::GetCurrentContext()->UserTextures.Size, before + 1);
    }
    // No backend ever made its side of the texture, so it goes at once.
    EXPECT(!Texture::Collect());
    EXPECT_EQ(ImGui::GetCurrentContext()->UserTextures.Size, before);
}
