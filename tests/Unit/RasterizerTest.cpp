#include "Graphics/Software/Rasterizer.h"

#include "Test.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <utility>
#include <vector>

class RasterizerTest {
protected:
    static constexpr int32_t Side = 16;
    static constexpr Region All{0, 0, Side, Side};
    static constexpr uint32_t Clear = 0;
    static constexpr uint32_t Black = Pixel::Rgba(0, 0, 0, 255);
    static constexpr uint32_t OpaqueWhite = Pixel::Rgba(255, 255, 255, 255);

    struct Vertex {
        float x, y, u, v;
        ImU32 color;
    };

    // The vertices and indices of one draw command, for a mesh to point into.
    struct Shape {
        std::vector<ImDrawVert> vertices;
        std::vector<ImDrawIdx> indices;

        Shape(std::initializer_list<Vertex> corners, std::initializer_list<ImDrawIdx> order) : indices(order) {
            for (const Vertex& corner : corners) {
                vertices.push_back({ImVec2(corner.x, corner.y), ImVec2(corner.u, corner.v), corner.color});
            }
        }

        Mesh ToMesh(Region clip = All) const {
            return {vertices, indices, 1, clip};
        }
    };

    // A rectangle as Dear ImGui's draw lists make one: the corners clockwise from the top left,
    // and two triangles over them.
    static Shape Rectangle(float left, float top, float right, float bottom, ImU32 color, std::array<float, 4> uv = {}) {
        return Shape({{left, top, uv[0], uv[1], color}, {right, top, uv[2], uv[1], color}, {right, bottom, uv[2], uv[3], color}, {left, bottom, uv[0], uv[3], color}}, {0, 1, 2, 0, 2, 3});
    }

    // A texture as Dear ImGui hands one over: four bytes a pixel, alpha apart.
    template <class Pixel>
    static SoftwareTexture TextureOf(int width, int height, Pixel pixel) {
        ImTextureData source;
        source.Create(ImTextureFormat_RGBA32, width, height);
        auto* bytes = static_cast<uint8_t*>(source.GetPixels());
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                const std::array<uint8_t, 4> color = pixel(x, y);
                std::ranges::copy(color, bytes + (y * width + x) * 4);
            }
        }
        SoftwareTexture texture;
        texture.Load(source);
        return texture;
    }

    static SoftwareTexture White() {
        return TextureOf(1, 1, [](int, int) { return std::array<uint8_t, 4>{255, 255, 255, 255}; });
    }

    static std::vector<uint32_t> Draw(std::initializer_list<Mesh> meshes, const SoftwareTexture& texture, uint32_t background = Clear) {
        std::vector<uint32_t> pixels(Side * Side, background);
        Canvas canvas(pixels, Side, All);
        for (const Mesh& mesh : meshes) {
            canvas.DrawMesh(mesh, texture);
        }
        return pixels;
    }

    static uint32_t At(const std::vector<uint32_t>& pixels, int x, int y, int stride = Side) {
        return pixels[static_cast<size_t>(y * stride + x)];
    }

    static uint32_t Channel(uint32_t pixel, int index) {
        return pixel >> (index * 8) & 0xff;
    }

    static std::vector<std::pair<int, int>> Covered(const std::vector<uint32_t>& pixels, uint32_t background) {
        std::vector<std::pair<int, int>> found;
        for (int index = 0; index < Side * Side; index++) {
            if (pixels[static_cast<size_t>(index)] != background) {
                found.emplace_back(index % Side, index / Side);
            }
        }
        return found;
    }

    // Whether no channel is above the alpha: what laying one colour over another relies on.
    static bool Premultiplied(std::span<const uint32_t> pixels) {
        return std::ranges::all_of(pixels, [](uint32_t pixel) { return std::max({Channel(pixel, 0), Channel(pixel, 1), Channel(pixel, 2)}) <= Channel(pixel, 3); });
    }

    struct Random {
        uint32_t state;

        uint32_t Below(uint32_t limit) {
            state = state * 1664525u + 1013904223u;
            return (state >> 8) % limit;
        }
    };

    // The pixels of a texture with their alpha apart, read the way a GPU does.
    struct Straight {
        int width;
        int height;
        std::vector<std::array<uint8_t, 4>> texels;

        SoftwareTexture Taken() const {
            return TextureOf(width, height, [this](int x, int y) { return texels[static_cast<size_t>(y * width + x)]; });
        }

        std::array<double, 4> Sample(double u, double v) const {
            const double x = u * width - 0.5, y = v * height - 0.5, left = std::floor(x), top = std::floor(y);
            std::array<double, 4> color{};
            for (size_t index = 0; index < 4; index++) {
                const auto texel = [&](double column, double row) { return static_cast<double>(texels[static_cast<size_t>(std::clamp(static_cast<int>(row), 0, height - 1) * width + std::clamp(static_cast<int>(column), 0, width - 1))][index]); };
                const double upper = std::lerp(texel(left, top), texel(left + 1, top), x - left), lower = std::lerp(texel(left, top + 1), texel(left + 1, top + 1), x - left);
                color[index] = std::lerp(upper, lower, y - top);
            }
            return color;
        }
    };

    struct Wanted {
        bool inside;
        std::array<double, 4> color;
    };

    // What a GPU leaves at one pixel after a triangle, worked out the slow way: where the centre
    // lies between the corners, the colour and the texture position weighted by that, four
    // texels filtered, and the alpha multiplied in when blending. Nothing where the centre is
    // too close to an edge to tell which side it is on.
    static std::optional<Wanted> Expected(const std::array<Vertex, 3>& corners, const Straight& texture, int x, int y, uint32_t background) {
        const double px = x + 0.5, py = y + 0.5;
        std::array<double, 3> weights{};
        double area = 0.0, nearest = 1e9;
        for (size_t index = 0; index < 3; index++) {
            const Vertex& from = corners[(index + 1) % 3];
            const Vertex& to = corners[(index + 2) % 3];
            const double across = double{to.x} - from.x, down = double{to.y} - from.y;
            weights[index] = across * (py - from.y) - down * (px - from.x);
            area += weights[index];
        }
        for (size_t index = 0; index < 3; index++) {
            const Vertex& from = corners[(index + 1) % 3];
            const Vertex& to = corners[(index + 2) % 3];
            nearest = std::min(nearest, weights[index] * (area < 0.0 ? -1.0 : 1.0) / std::hypot(double{to.x} - from.x, double{to.y} - from.y));
        }
        if (std::fabs(nearest) < 1e-3) {
            return std::nullopt;
        }

        std::array<double, 4> result{};
        for (int index = 0; index < 4; index++) {
            result[static_cast<size_t>(index)] = Channel(background, index);
        }
        if (nearest < 0.0) {
            return Wanted{false, result};
        }

        double u = 0.0, v = 0.0;
        std::array<double, 4> color{};
        for (size_t corner = 0; corner < 3; corner++) {
            const double weight = weights[corner] / area;
            u += weight * corners[corner].u;
            v += weight * corners[corner].v;
            const ImU32 packed = corners[corner].color;
            const std::array<ImU32, 4> channels{packed >> IM_COL32_R_SHIFT & 0xff, packed >> IM_COL32_G_SHIFT & 0xff, packed >> IM_COL32_B_SHIFT & 0xff, packed >> IM_COL32_A_SHIFT & 0xff};
            for (size_t index = 0; index < 4; index++) {
                color[index] += weight * channels[index];
            }
        }
        const std::array<double, 4> texel = texture.Sample(u, v);
        const double alpha = color[3] * texel[3] / 255.0;
        for (size_t index = 0; index < 3; index++) {
            result[index] = color[index] * texel[index] / 255.0 * alpha / 255.0 + result[index] * (1.0 - alpha / 255.0);
        }
        result[3] = alpha + result[3] * (1.0 - alpha / 255.0);
        return Wanted{true, result};
    }

    // The same after the two triangles of a rectangle.
    static std::optional<Wanted> Expected(const std::array<Vertex, 4>& corners, const Straight& texture, int x, int y, uint32_t background) {
        const std::optional<Wanted> first = Expected(std::array{corners[0], corners[1], corners[2]}, texture, x, y, background);
        const std::optional<Wanted> second = Expected(std::array{corners[0], corners[2], corners[3]}, texture, x, y, background);
        if (!first || !second) {
            return std::nullopt;
        }
        return first->inside ? first : second;
    }

    // How far what was drawn lies from what a GPU would leave, over every pixel that can be told.
    struct Agreement {
        int checked = 0;
        int inside = 0;
        double worst = 0.0;

        template <size_t N>
        void Compare(const std::array<Vertex, N>& corners, const Straight& texture, const std::vector<uint32_t>& pixels, uint32_t background) {
            for (int y = 0; y < Side; y++) {
                for (int x = 0; x < Side; x++) {
                    const std::optional<Wanted> wanted = Expected(corners, texture, x, y, background);
                    if (!wanted) {
                        continue;
                    }
                    checked++;
                    if (wanted->inside) {
                        inside++;
                    }
                    const uint32_t drawn = At(pixels, x, y);
                    for (int index = 0; index < 4; index++) {
                        worst = std::max(worst, std::fabs(Channel(drawn, index) - wanted->color[static_cast<size_t>(index)]));
                    }
                }
            }
            if (!Premultiplied(pixels)) {
                worst = 255.0;
            }
        }
    };

    static void CountCall(const ImDrawList*, const ImDrawCmd* called) {
        ++*static_cast<int*>(called->UserCallbackData);
    }

    static ImDrawCmd Drawing(ImVec4 clip, ImTextureRef drawnWith, unsigned firstVertex, unsigned firstIndex, unsigned indices) {
        ImDrawCmd made;
        made.ClipRect = clip;
        made.TexRef = drawnWith;
        made.VtxOffset = firstVertex;
        made.IdxOffset = firstIndex;
        made.ElemCount = indices;
        return made;
    }
};

TEST(RasterizerTest, "dividing by 255 rounds like the exact division") {
    int wrong = 0;
    for (uint32_t value = 0; value <= 255 * 255; value++) {
        if (Pixel::Div255(value) != static_cast<uint32_t>(std::lround(value / 255.0))) {
            wrong++;
        }
    }
    EXPECT_EQ(wrong, 0);
    EXPECT_EQ(Pixel::Over(Pixel::Rgba(100, 50, 0, 200), Pixel::Rgba(128, 0, 0, 128)), Pixel::Rgba(114, 25, 0, 228));
}

TEST(RasterizerTest, "laying a colour over a pixel two channels at a time is laying each channel") {
    Random random{7};
    int wrong = 0;
    for (int round = 0; round < 100000; round++) {
        const std::array<uint32_t, 4> color{random.Below(256), random.Below(256), random.Below(256), random.Below(256)};
        const std::array<uint32_t, 4> pixel{random.Below(256), random.Below(256), random.Below(256), random.Below(256)};
        uint32_t each = 0;
        for (size_t index = 0; index < 4; index++) {
            each |= Pixel::Div255((index == 3 ? 255 : color[index]) * color[3] + pixel[index] * (255 - color[3])) << (index * 8);
        }
        if (Pixel::Over(Pixel::Rgba(pixel[0], pixel[1], pixel[2], pixel[3]), Pixel::Rgba(color[0], color[1], color[2], color[3])) != each) {
            wrong++;
        }
    }
    EXPECT_EQ(wrong, 0);
    EXPECT_EQ(Pixel::Over(OpaqueWhite, Pixel::Rgba(1, 2, 3, 0)), OpaqueWhite);
    EXPECT_EQ(Pixel::Over(OpaqueWhite, Pixel::Rgba(1, 2, 3, 255)), Pixel::Rgba(1, 2, 3, 255));
}

TEST(RasterizerTest, "colours with their alpha apart become premultiplied pixels") {
    EXPECT_EQ(Pixel::Unpack(IM_COL32(1, 2, 3, 4)), Pixel::Rgba(1, 2, 3, 4));
    EXPECT_EQ(Pixel::Over(Clear, Pixel::Rgba(255, 128, 0, 128)), Pixel::Rgba(128, 64, 0, 128));
    EXPECT_EQ(Pixel::Over(Clear, Pixel::Rgba(10, 20, 30, 255)), Pixel::Rgba(10, 20, 30, 255));
    EXPECT_EQ(Pixel::Over(Clear, Pixel::Rgba(255, 255, 255, 0)), Clear);
    EXPECT_EQ(Pixel::Modulate(Pixel::Rgba(128, 64, 0, 128), OpaqueWhite), Pixel::Rgba(128, 64, 0, 128));
    EXPECT_EQ(Pixel::Modulate(Pixel::Rgba(255, 128, 0, 128), Pixel::Rgba(128, 128, 255, 255)), Pixel::Rgba(128, 64, 0, 128));
    int wrong = 0;
    for (uint32_t color = 0; color < 256; color++) {
        for (uint32_t alpha = 0; alpha < 256; alpha++) {
            const uint32_t each = Pixel::Rgba(Pixel::Div255(color * alpha), Pixel::Div255((255 - color) * alpha), Pixel::Div255(color / 2 * alpha), alpha);
            if (Pixel::Over(Clear, Pixel::Rgba(color, 255 - color, color / 2, alpha)) != each) {
                wrong++;
            }
        }
    }
    EXPECT_EQ(wrong, 0);

    const Shape half = Rectangle(2.0f, 2.0f, 6.0f, 6.0f, IM_COL32(255, 128, 0, 128));
    EXPECT_EQ(At(Draw({half.ToMesh()}, White()), 3, 3), Pixel::Rgba(128, 64, 0, 128));
    EXPECT_EQ(At(Draw({half.ToMesh()}, White(), Pixel::Rgba(0, 0, 200, 255)), 3, 3), Pixel::Rgba(128, 64, 100, 255));
    const Shape triangle({{2.0f, 2.0f, 0.0f, 0.0f, IM_COL32(255, 128, 0, 128)}, {12.0f, 2.0f, 0.0f, 0.0f, IM_COL32(255, 128, 0, 128)}, {2.0f, 12.0f, 0.0f, 0.0f, IM_COL32(255, 128, 0, 128)}}, {0, 1, 2});
    EXPECT_EQ(At(Draw({triangle.ToMesh()}, White()), 3, 3), Pixel::Rgba(128, 64, 0, 128));
}

TEST(RasterizerTest, "a colour shaded towards transparency is multiplied by its alpha pixel by pixel") {
    const Shape fading({{0.0f, 0.0f, 0.0f, 0.0f, IM_COL32(255, 0, 0, 255)}, {16.0f, 0.0f, 0.0f, 0.0f, IM_COL32(0, 0, 255, 0)}, {0.0f, 16.0f, 0.0f, 0.0f, IM_COL32(255, 0, 0, 255)}}, {0, 1, 2});
    const std::vector<uint32_t> pixels = Draw({fading.ToMesh()}, White());
    for (const int x : {0, 4, 9}) {
        const double along = (x + 0.5) / 16.0, alpha = 255.0 * (1.0 - along);
        const uint32_t pixel = At(pixels, x, 2);
        EXPECT_NEAR(Channel(pixel, 0), 255.0 * (1.0 - along) * alpha / 255.0, 1.0);
        EXPECT_NEAR(Channel(pixel, 2), 255.0 * along * alpha / 255.0, 1.0);
        EXPECT_NEAR(Channel(pixel, 3), alpha, 0.5);
    }
    EXPECT(Premultiplied(pixels));

    // The edge of an anti-aliased shape: one colour, fading out.
    const Shape fringe({{0.0f, 0.0f, 0.0f, 0.0f, IM_COL32(200, 100, 50, 255)}, {16.0f, 0.0f, 0.0f, 0.0f, IM_COL32(200, 100, 50, 0)}, {0.0f, 16.0f, 0.0f, 0.0f, IM_COL32(200, 100, 50, 255)}}, {0, 1, 2});
    const uint32_t pixel = At(Draw({fringe.ToMesh()}, White(), Black), 7, 1);
    EXPECT_EQ(Channel(pixel, 3), 255u);
    EXPECT_NEAR(Channel(pixel, 0), 200.0 * (1.0 - 7.5 / 16.0), 1.0);
    EXPECT_NEAR(Channel(pixel, 1), 100.0 * (1.0 - 7.5 / 16.0), 1.0);
}

TEST(RasterizerTest, "a rectangle covers the pixels whose centres it contains") {
    const std::vector<uint32_t> pixels = Draw({Rectangle(2.4f, 3.6f, 5.5f, 6.5f, IM_COL32(10, 20, 30, 255)).ToMesh()}, White());
    std::vector<std::pair<int, int>> wanted;
    for (int y = 4; y < 6; y++) {
        for (int x = 2; x < 5; x++) {
            wanted.emplace_back(x, y);
        }
    }
    EXPECT(Covered(pixels, Clear) == wanted);
    EXPECT_EQ(At(pixels, 2, 4), Pixel::Rgba(10, 20, 30, 255));
}

TEST(RasterizerTest, "triangles sharing an edge leave no seam and no overlap") {
    const ImU32 red = IM_COL32(255, 0, 0, 100), alsoRed = IM_COL32(255, 0, 0, 101);
    const Shape quad({{1.3f, 1.7f, 0.0f, 0.0f, red}, {13.2f, 2.1f, 0.0f, 0.0f, red}, {14.9f, 14.4f, 0.0f, 0.0f, red}, {2.2f, 12.6f, 0.0f, 0.0f, alsoRed}}, {0, 1, 2, 0, 2, 3});
    std::vector<uint32_t> pixels = Draw({quad.ToMesh()}, White(), Black);
    const std::vector<std::pair<int, int>> touched = Covered(pixels, Black);
    EXPECT(touched.size() > 100);
    int blendedTwice = 0;
    for (const auto& [x, y] : touched) {
        if (Channel(At(pixels, x, y), 0) < 100 || Channel(At(pixels, x, y), 0) > 101) {
            blendedTwice++;
        }
    }
    EXPECT_EQ(blendedTwice, 0);

    const Shape fan({{8.0f, 8.0f, 0.0f, 0.0f, red}, {1.0f, 1.0f, 0.0f, 0.0f, red}, {15.0f, 1.0f, 0.0f, 0.0f, red}, {15.0f, 15.0f, 0.0f, 0.0f, red}, {1.0f, 15.0f, 0.0f, 0.0f, red}}, {0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 1});
    pixels = Draw({fan.ToMesh()}, White(), Black);
    EXPECT_EQ(Covered(pixels, Black).size(), size_t{14 * 14});
    EXPECT(std::ranges::all_of(pixels, [](uint32_t pixel) { return Channel(pixel, 0) == 0 || Channel(pixel, 0) == 100; }));
}

TEST(RasterizerTest, "colours shade smoothly across a triangle") {
    const Shape shape({{0.0f, 0.0f, 0.0f, 0.0f, IM_COL32(0, 0, 0, 255)}, {16.0f, 0.0f, 0.0f, 0.0f, IM_COL32(160, 0, 0, 255)}, {0.0f, 16.0f, 0.0f, 0.0f, IM_COL32(0, 160, 0, 255)}}, {0, 1, 2});
    const std::vector<uint32_t> pixels = Draw({shape.ToMesh()}, White());
    EXPECT_EQ(At(pixels, 0, 0), Pixel::Rgba(5, 5, 0, 255));
    EXPECT_EQ(At(pixels, 8, 3), Pixel::Rgba(85, 35, 0, 255));
    EXPECT_EQ(At(pixels, 15, 15), Clear);
}

TEST(RasterizerTest, "glyphs are copied texel for texel and tinted") {
    const SoftwareTexture atlas = TextureOf(8, 8, [](int x, int y) { return std::array<uint8_t, 4>{static_cast<uint8_t>(x * 30), static_cast<uint8_t>(y * 30), 0, 255}; });
    const auto quad = [](ImU32 tint) { return Rectangle(3.0f, 5.0f, 7.0f, 8.0f, tint, {2.0f / 8.0f, 1.0f / 8.0f, 6.0f / 8.0f, 4.0f / 8.0f}); };
    std::vector<uint32_t> pixels = Draw({quad(IM_COL32_WHITE).ToMesh()}, atlas);
    EXPECT_EQ(Covered(pixels, Clear).size(), size_t{4 * 3});
    EXPECT_EQ(At(pixels, 3, 5), Pixel::Rgba(60, 30, 0, 255));
    EXPECT_EQ(At(pixels, 6, 7), Pixel::Rgba(150, 90, 0, 255));

    pixels = Draw({quad(IM_COL32(255, 255, 255, 128)).ToMesh()}, atlas);
    EXPECT_EQ(At(pixels, 6, 7), Pixel::Rgba(75, 45, 0, 128));

    // A font texture is white and its alpha is the glyph: the tint shows through it.
    const SoftwareTexture font = TextureOf(8, 8, [](int x, int) { return std::array<uint8_t, 4>{255, 255, 255, static_cast<uint8_t>(x * 36)}; });
    pixels = Draw({quad(IM_COL32(255, 128, 0, 255)).ToMesh()}, font);
    EXPECT_EQ(At(pixels, 3, 5), Pixel::Rgba(72, 36, 0, 72));
    EXPECT_EQ(At(pixels, 6, 7), Pixel::Rgba(180, 90, 0, 180));
}

TEST(RasterizerTest, "glyphs off the pixel grid or stretched are filtered as a gpu filters them") {
    // Around the glyph the font texture is clear and black. A GPU filters every channel by
    // itself, alpha among them, so what it reads between the two is half as bright and half as
    // opaque: a quarter of the colour is left once it blends.
    const SoftwareTexture font = TextureOf(8, 2, [](int x, int) { return x >= 2 && x < 6 ? std::array<uint8_t, 4>{255, 255, 255, 200} : std::array<uint8_t, 4>{0, 0, 0, 0}; });
    const std::array<float, 4> glyph{2.0f / 8.0f, 0.0f, 6.0f / 8.0f, 1.0f};
    std::vector<uint32_t> pixels = Draw({Rectangle(4.5f, 2.0f, 8.5f, 4.0f, IM_COL32_WHITE, glyph).ToMesh()}, font);
    EXPECT_EQ(Covered(pixels, Clear).size(), size_t{4 * 2});
    EXPECT_EQ(At(pixels, 4, 2), Pixel::Rgba(50, 50, 50, 100));
    EXPECT_EQ(At(pixels, 5, 2), Pixel::Rgba(200, 200, 200, 200));
    EXPECT_EQ(At(pixels, 7, 3), Pixel::Rgba(200, 200, 200, 200));
    EXPECT_EQ(At(pixels, 8, 2), Clear);

    // A GPU keeps positions on a grid of 1/256 of a pixel. A glyph a hair past the middle of a
    // pixel is on the middle there, and covers the pixels it would cover from exactly there.
    pixels = Draw({Rectangle(4.5008f, 2.0f, 8.5008f, 4.0f, IM_COL32_WHITE, glyph).ToMesh()}, font);
    EXPECT_EQ(Covered(pixels, Clear).size(), size_t{4 * 2});
    EXPECT_EQ(At(pixels, 4, 2), Pixel::Rgba(50, 50, 50, 100));
    EXPECT_EQ(At(pixels, 8, 2), Clear);
    // Far enough past it, the glyph has moved on by a pixel.
    pixels = Draw({Rectangle(4.51f, 2.0f, 8.51f, 4.0f, IM_COL32_WHITE, glyph).ToMesh()}, font);
    EXPECT_EQ(At(pixels, 4, 2), Clear);
    EXPECT(At(pixels, 8, 2) != Clear);

    // Dear ImGui keeps small glyphs twice as wide in the texture as they are on the screen,
    // and puts them down at fractions of a pixel.
    pixels = Draw({Rectangle(4.0f, 2.0f, 6.0f, 4.0f, IM_COL32(255, 0, 0, 255), glyph).ToMesh()}, font);
    EXPECT_EQ(Covered(pixels, Clear).size(), size_t{2 * 2});
    EXPECT_EQ(At(pixels, 4, 2), Pixel::Rgba(200, 0, 0, 200));
    EXPECT_EQ(At(pixels, 5, 3), Pixel::Rgba(200, 0, 0, 200));
    pixels = Draw({Rectangle(3.625f, 2.0f, 5.625f, 4.0f, IM_COL32(255, 0, 0, 255), glyph).ToMesh()}, font);
    EXPECT_EQ(At(pixels, 3, 2), Clear);
    EXPECT_EQ(At(pixels, 4, 2), Pixel::Rgba(200, 0, 0, 200));
    EXPECT_EQ(At(pixels, 5, 2), Pixel::Rgba(112, 0, 0, 150));
    EXPECT_EQ(At(pixels, 6, 2), Clear);
}

TEST(RasterizerTest, "stretched textures are filtered") {
    const SoftwareTexture checker = TextureOf(2, 1, [](int x, int) { return x == 0 ? std::array<uint8_t, 4>{0, 0, 0, 255} : std::array<uint8_t, 4>{200, 200, 200, 255}; });
    const std::vector<uint32_t> smooth = Draw({Rectangle(0.0f, 0.0f, 16.0f, 4.0f, IM_COL32_WHITE, {0.0f, 0.0f, 1.0f, 1.0f}).ToMesh()}, checker);
    EXPECT_EQ(Channel(smooth[0], 0), 0u);
    EXPECT_EQ(Channel(smooth[15], 0), 200u);
    EXPECT(Channel(smooth[8], 0) >= 105 && Channel(smooth[8], 0) <= 120);
    EXPECT(Channel(smooth[7], 0) > 0 && Channel(smooth[7], 0) < Channel(smooth[8], 0));
}

TEST(RasterizerTest, "nothing is drawn outside the clip or the canvas area") {
    const Shape everything = Rectangle(-50.0f, -50.0f, 50.0f, 50.0f, IM_COL32(9, 9, 9, 255));
    const Region clip{4, 5, 9, 7};
    EXPECT_EQ(Covered(Draw({everything.ToMesh(clip)}, White()), Clear).size(), size_t{5 * 2});
    const Shape beside = Rectangle(10.0f, 0.0f, 14.0f, 16.0f, IM_COL32(9, 9, 9, 255));
    EXPECT(Covered(Draw({beside.ToMesh(clip)}, White()), Clear).empty());

    const uint32_t before = Pixel::Rgba(1, 1, 1, 255);
    std::vector<uint32_t> pixels(Side * Side, before);
    const std::vector<Canvas> bands = Canvas(pixels, Side, Region{2, 3, 10, 12}).Bands(4);
    int rows = 0;
    for (Canvas band : bands) {
        rows += band.GetArea().bottom - band.GetArea().top;
        band.Clear();
        band.DrawMesh(everything.ToMesh(), White());
    }
    EXPECT_EQ(rows, 9);
    EXPECT_EQ(Covered(pixels, before).size(), size_t{8 * 9});
    EXPECT_EQ(At(pixels, 2, 3), Pixel::Rgba(9, 9, 9, 255));
    EXPECT_EQ(At(pixels, 2, 2), before);
    EXPECT_EQ(At(pixels, 9, 12), before);

    EXPECT(Canvas(pixels, Side, Region{-5, -5, 40, 40}).GetArea() == All);
    EXPECT(Canvas(pixels, Side, Region{6, 6, 6, 9}).Bands(4).empty());
}

TEST(RasterizerTest, "damage is where the picture changes") {
    const Region screen{0, 0, 200, 100};
    const Shape fixed = Rectangle(150.0f, 60.0f, 190.0f, 90.0f, IM_COL32_WHITE);
    const auto moving = [](float x) { return Rectangle(x, 10.0f, x + 8.0f, 18.0f, IM_COL32_WHITE); };
    const std::array<ImTextureID, 1> texture{1};
    Damage damage;
    const auto frame = [&](const Shape& moved, bool repainted) {
        const std::array meshes{fixed.ToMesh(screen), moved.ToMesh(screen)};
        return damage.Update(screen, meshes, repainted ? std::span<const ImTextureID>(texture) : std::span<const ImTextureID>());
    };
    const auto still = [&](Mesh mesh) { return damage.Update(screen, std::span<const Mesh>(&mesh, 1), {}); };

    EXPECT(frame(moving(4.0f), false) == screen);
    EXPECT(frame(moving(4.0f), false).IsEmpty());
    EXPECT(frame(moving(40.0f), false) == (Region{0, 0, 64, 32}));
    EXPECT(frame(moving(41.0f), false) == (Region{32, 0, 64, 32}));
    EXPECT(frame(moving(41.0f), true) == (Region{32, 0, 192, 96}));
    EXPECT(still(fixed.ToMesh(screen)) == (Region{32, 0, 192, 96}));
    EXPECT(still(fixed.ToMesh(screen)) == Region{});

    damage.Reset();
    EXPECT(still(fixed.ToMesh(screen)) == screen);
    EXPECT(still(fixed.ToMesh(Region{0, 0, 160, 100})) == (Region{128, 32, 192, 96}));
}

TEST(RasterizerTest, "a frame moved or scaled as a whole is damage too") {
    const Region screen{0, 0, 200, 100};
    const Shape shape = Rectangle(70.0f, 40.0f, 90.0f, 60.0f, IM_COL32_WHITE);
    Damage damage;
    const auto frame = [&](ImVec2 origin, ImVec2 zoom) {
        Mesh mesh = shape.ToMesh(screen);
        mesh.origin = origin;
        mesh.scale = zoom;
        return damage.Update(screen, std::span<const Mesh>(&mesh, 1), {});
    };
    EXPECT(frame(ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f)) == screen);
    EXPECT(frame(ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f)).IsEmpty());
    EXPECT(frame(ImVec2(1.0f, 0.0f), ImVec2(1.0f, 1.0f)) == (Region{64, 32, 96, 64}));
    EXPECT(frame(ImVec2(1.0f, 0.0f), ImVec2(2.0f, 2.0f)) == (Region{64, 32, 192, 100}));
}

TEST(RasterizerTest, "a patch updates part of a texture") {
    ImTextureData source;
    source.Create(ImTextureFormat_RGBA32, 4, 4);
    auto* bytes = static_cast<uint8_t*>(source.GetPixels());
    for (int index = 0; index < 4 * 4; index++) {
        bytes[index * 4 + 3] = 255;
    }
    SoftwareTexture atlas;
    atlas.Load(source);
    std::fill_n(bytes, 4 * 4 * 4, uint8_t{255});
    atlas.Patch(source, ImTextureRect{1, 2, 2, 2});
    EXPECT_EQ(atlas.Texel(1, 2), OpaqueWhite);
    EXPECT_EQ(atlas.Texel(2, 3), OpaqueWhite);
    EXPECT_EQ(atlas.Texel(0, 2), Black);
    EXPECT_EQ(atlas.Texel(3, 3), Black);
    atlas.Patch(source, ImTextureRect{3, 3, 40, 40});
    EXPECT_EQ(atlas.Texel(3, 3), OpaqueWhite);
    EXPECT_EQ(atlas.Texel(9, 9), OpaqueWhite);
    EXPECT_EQ(atlas.Texel(-3, 0), Black);

    EXPECT(Mesh::ClipRegion(ImVec4(1.0f, 1.0f, 3.0f, 4.0f), ImVec2(0.0f, 0.0f), ImVec2(2.25f, 2.25f)) == (Region{2, 2, 7, 9}));
    EXPECT(Mesh::ClipRegion(ImVec4(101.0f, 51.0f, 103.0f, 54.0f), ImVec2(100.0f, 50.0f), ImVec2(2.0f, 3.0f)) == (Region{2, 3, 6, 12}));
    const Region endless = Mesh::ClipRegion(ImVec4(-1e30f, -1e30f, 1e30f, 1e30f), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
    EXPECT(endless.Intersect(All) == All);
}

TEST(RasterizerTest, "textures are created updated and destroyed as dear imgui asks") {
    SoftwareTextures textures;
    ImTextureData colours;
    colours.Create(ImTextureFormat_RGBA32, 2, 2);
    auto* bytes = static_cast<uint8_t*>(colours.GetPixels());
    std::ranges::copy(std::array<uint8_t, 16>{255, 128, 0, 128, 10, 20, 30, 255, 200, 200, 200, 0, 0, 0, 0, 0}, bytes);
    EXPECT(!textures.Update(colours));
    EXPECT(colours.Status == ImTextureStatus_OK);
    EXPECT(colours.GetTexID() != ImTextureID_Invalid);
    const SoftwareTexture* kept = textures.Find(colours.GetTexID());
    EXPECT(kept && kept->GetWidth() == 2 && kept->GetHeight() == 2);
    if (!kept) {
        return;
    }
    EXPECT_EQ(kept->Texel(0, 0), Pixel::Rgba(255, 128, 0, 128));
    EXPECT_EQ(kept->Texel(1, 0), Pixel::Rgba(10, 20, 30, 255));
    EXPECT_EQ(kept->Texel(0, 1), Pixel::Rgba(200, 200, 200, 0));

    ImTextureData glyphs;
    glyphs.Create(ImTextureFormat_Alpha8, 3, 1);
    std::ranges::copy(std::array<uint8_t, 3>{0, 100, 255}, static_cast<uint8_t*>(glyphs.GetPixels()));
    EXPECT(!textures.Update(glyphs));
    EXPECT(glyphs.Status == ImTextureStatus_OK && glyphs.GetTexID() != colours.GetTexID());
    const SoftwareTexture* font = textures.Find(glyphs.GetTexID());
    EXPECT(font != nullptr);
    if (!font) {
        return;
    }
    EXPECT_EQ(font->Texel(0, 0), Pixel::Rgba(255, 255, 255, 0));
    EXPECT_EQ(font->Texel(1, 0), Pixel::Rgba(255, 255, 255, 100));
    EXPECT_EQ(font->Texel(2, 0), OpaqueWhite);

    // The rectangles of an update are taken, and nothing beside them.
    std::fill_n(bytes, 16, uint8_t{255});
    colours.Updates.push_back(ImTextureRect{1, 1, 1, 1});
    colours.SetStatus(ImTextureStatus_WantUpdates);
    EXPECT(textures.Update(colours));
    EXPECT(colours.Status == ImTextureStatus_OK);
    EXPECT_EQ(kept->Texel(1, 1), OpaqueWhite);
    EXPECT_EQ(kept->Texel(0, 0), Pixel::Rgba(255, 128, 0, 128));

    colours.Updates.clear();
    colours.UpdateRect = ImTextureRect{0, 0, 2, 1};
    colours.SetStatus(ImTextureStatus_WantUpdates);
    EXPECT(textures.Update(colours));
    EXPECT_EQ(kept->Texel(0, 0), OpaqueWhite);
    EXPECT_EQ(kept->Texel(0, 1), Pixel::Rgba(200, 200, 200, 0));

    // A frame's list of textures is gone through; what changed under the eyes is named.
    std::ranges::copy(std::array<uint8_t, 3>{9, 9, 9}, static_cast<uint8_t*>(glyphs.GetPixels()));
    glyphs.Updates.push_back(ImTextureRect{0, 0, 3, 1});
    glyphs.SetStatus(ImTextureStatus_WantUpdates);
    ImVector<ImTextureData*> listed;
    listed.push_back(&colours);
    listed.push_back(&glyphs);
    ImDrawData frame;
    EXPECT(textures.Update(frame).empty());
    frame.Textures = &listed;
    EXPECT(textures.Update(frame) == std::vector{glyphs.GetTexID()});
    EXPECT_EQ(font->Texel(1, 0), Pixel::Rgba(255, 255, 255, 9));

    // A texture still in use is not destroyed yet.
    glyphs.WantDestroyNextFrame = true;
    glyphs.SetStatus(ImTextureStatus_WantDestroy);
    EXPECT(!textures.Update(glyphs));
    EXPECT(glyphs.Status == ImTextureStatus_WantDestroy && textures.Find(glyphs.GetTexID()) != nullptr);
    glyphs.UnusedFrames = 1;
    const ImTextureID gone = glyphs.GetTexID();
    EXPECT(!textures.Update(glyphs));
    EXPECT(glyphs.Status == ImTextureStatus_Destroyed && glyphs.GetTexID() == ImTextureID_Invalid);
    EXPECT(textures.Find(gone) == nullptr);

    // A backend that ends leaves the texture to be asked for again.
    textures.Destroy(colours);
    EXPECT(colours.Status == ImTextureStatus_WantCreate && colours.GetTexID() == ImTextureID_Invalid);
    EXPECT_EQ(textures.GetSize(), size_t{0});
    EXPECT(!textures.Update(colours));
    EXPECT(colours.GetTexID() != gone && textures.GetSize() == 1);
}

TEST(RasterizerTest, "a frame is taken command by command") {
    SoftwareTextures textures;
    ImTextureData first, second;
    for (ImTextureData* texture : {&first, &second}) {
        texture->Create(ImTextureFormat_RGBA32, 1, 1);
        std::fill_n(static_cast<uint8_t*>(texture->GetPixels()), 4, uint8_t{255});
        textures.Update(*texture);
    }

    const ImVec2 origin(100.0f, 50.0f);
    const ImU32 red = IM_COL32(255, 0, 0, 255), green = IM_COL32(0, 255, 0, 255);
    const Shape left = Rectangle(101.0f, 51.0f, 105.0f, 55.0f, red), right = Rectangle(106.0f, 50.0f, 114.0f, 60.0f, green);
    ImDrawList list(nullptr);
    for (const Shape* shape : {&left, &right}) {
        for (const ImDrawVert& vertex : shape->vertices) {
            list.VtxBuffer.push_back(vertex);
        }
        for (const ImDrawIdx index : shape->indices) {
            list.IdxBuffer.push_back(index);
        }
    }
    int called = 0;
    ImDrawCmd callback, reset;
    callback.UserCallback = CountCall;
    callback.UserCallbackData = &called;
    reset.UserCallback = ImDrawCallback_ResetRenderState;
    list.CmdBuffer.push_back(Drawing(ImVec4(100.0f, 50.0f, 116.0f, 66.0f), ImTextureRef(first.GetTexID()), 0, 0, 6));
    list.CmdBuffer.push_back(callback);
    list.CmdBuffer.push_back(reset);
    list.CmdBuffer.push_back(Drawing(ImVec4(0.0f, 0.0f, 50.0f, 50.0f), ImTextureRef(first.GetTexID()), 0, 0, 6));
    list.CmdBuffer.push_back(Drawing(ImVec4(108.0f, 52.0f, 112.0f, 66.0f), second.GetTexRef(), 4, 6, 6));

    ImDrawData frame;
    frame.DisplayPos = origin;
    frame.DisplaySize = ImVec2(16.0f, 16.0f);
    frame.FramebufferScale = ImVec2(1.0f, 1.0f);
    frame.CmdLists.push_back(&list);
    frame.CmdListsCount = 1;

    const std::vector<Mesh> found = Mesh::Collect(frame, All);
    EXPECT_EQ(called, 1);
    EXPECT_EQ(found.size(), size_t{2});
    if (found.size() != 2) {
        return;
    }
    EXPECT(found[0].texture == first.GetTexID() && found[0].clip == All && found[0].indices.size() == 6);
    EXPECT(found[1].texture == second.GetTexID() && found[1].clip == (Region{8, 2, 12, 16}));
    EXPECT(found[1].vertices.data() == list.VtxBuffer.Data + 4 && found[1].indices.data() == list.IdxBuffer.Data + 6 && found[1].indices.size() == 6);

    std::vector<uint32_t> pixels(Side * Side, Clear);
    Canvas(pixels, Side, All).DrawMeshes(found, textures);
    EXPECT_EQ(Covered(pixels, Clear).size(), size_t{4 * 4 + 4 * 8});
    EXPECT_EQ(At(pixels, 1, 1), Pixel::Rgba(255, 0, 0, 255));
    EXPECT_EQ(At(pixels, 4, 4), Pixel::Rgba(255, 0, 0, 255));
    EXPECT_EQ(At(pixels, 8, 2), Pixel::Rgba(0, 255, 0, 255));
    EXPECT_EQ(At(pixels, 11, 9), Pixel::Rgba(0, 255, 0, 255));
    EXPECT_EQ(At(pixels, 7, 5), Clear);
    EXPECT_EQ(At(pixels, 9, 1), Clear);

    // A mesh whose texture is gone is left out; a frame with twice the pixels is twice the size.
    textures.Destroy(first);
    frame.DisplaySize = ImVec2(8.0f, 8.0f);
    frame.FramebufferScale = ImVec2(2.0f, 2.0f);
    list.CmdBuffer[4].ClipRect = ImVec4(100.0f, 50.0f, 105.5f, 58.0f);
    std::ranges::fill(pixels, Clear);
    Canvas(pixels, Side, All).DrawMeshes(Mesh::Collect(frame, All), textures);
    EXPECT_EQ(called, 2);
    EXPECT_EQ(Covered(pixels, Clear).size(), size_t{0});
    list.CmdBuffer[4].ClipRect = ImVec4(100.0f, 50.0f, 107.5f, 58.0f);
    Canvas(pixels, Side, All).DrawMeshes(Mesh::Collect(frame, All), textures);
    EXPECT_EQ(Covered(pixels, Clear).size(), size_t{3 * 16});
    EXPECT_EQ(At(pixels, 12, 0), Pixel::Rgba(0, 255, 0, 255));
    EXPECT_EQ(At(pixels, 11, 0), Clear);
    EXPECT_EQ(At(pixels, 15, 0), Clear);
}

TEST(RasterizerTest, "positions have the origin taken off and are scaled") {
    const Shape shape = Rectangle(102.4f, 203.6f, 105.5f, 206.5f, IM_COL32(10, 20, 30, 255));
    Mesh mesh = shape.ToMesh();
    mesh.origin = ImVec2(100.0f, 200.0f);
    std::vector<std::pair<int, int>> wanted;
    for (int y = 4; y < 6; y++) {
        for (int x = 2; x < 5; x++) {
            wanted.emplace_back(x, y);
        }
    }
    EXPECT(Covered(Draw({mesh}, White()), Clear) == wanted);

    mesh.scale = ImVec2(2.0f, 3.0f);
    const std::vector<uint32_t> pixels = Draw({mesh}, White());
    EXPECT_EQ(Covered(pixels, Clear).size(), size_t{6 * 5});
    EXPECT(At(pixels, 5, 11) != Clear && At(pixels, 10, 15) != Clear && At(pixels, 4, 11) == Clear && At(pixels, 11, 15) == Clear && At(pixels, 5, 10) == Clear);

    const Shape corner({{100.0f, 200.0f, 0.0f, 0.0f, IM_COL32_WHITE}, {108.0f, 200.0f, 0.0f, 0.0f, IM_COL32_WHITE}, {100.0f, 208.0f, 0.0f, 0.0f, IM_COL32_WHITE}}, {0, 1, 2});
    mesh = corner.ToMesh();
    mesh.origin = ImVec2(100.0f, 200.0f);
    mesh.scale = ImVec2(2.0f, 2.0f);
    EXPECT_EQ(Covered(Draw({mesh}, White()), Clear).size(), size_t{16 * 17 / 2 - 16});
}

TEST(RasterizerTest, "triangles are filled as a gpu fills them") {
    Random random{2026};
    const auto byte = [&] { return static_cast<uint8_t>(random.Below(256)); };
    Straight picture{4, 4, {}}, glyphs{4, 4, {}}, plain{1, 1, {{255, 255, 255, 255}}};
    for (int index = 0; index < 16; index++) {
        picture.texels.push_back({byte(), byte(), byte(), byte()});
        glyphs.texels.push_back(index % 4 == 0 ? std::array<uint8_t, 4>{} : std::array<uint8_t, 4>{255, 255, 255, byte()});
    }

    Agreement agreement;
    for (int round = 0; round < 600; round++) {
        const Straight& straight = round % 3 == 0 ? plain : round % 3 == 1 ? picture : glyphs;
        const bool oneColour = round / 3 % 2 == 0, oneTexel = round / 6 % 2 == 0;
        const uint32_t background = round / 12 % 2 == 0 ? Pixel::Rgba(40, 80, 120, 255) : Pixel::Rgba(20, 30, 40, 100);
        std::array<Vertex, 3> corners{};
        for (Vertex& corner : corners) {
            const std::array<uint8_t, 4> color{byte(), byte(), byte(), byte()};
            corner = {static_cast<float>(random.Below(8 * 28)) / 8.0f - 6.0f, static_cast<float>(random.Below(8 * 28)) / 8.0f - 6.0f, static_cast<float>(random.Below(17)) / 16.0f, static_cast<float>(random.Below(17)) / 16.0f, IM_COL32(color[0], color[1], color[2], color[3])};
            if (oneColour) {
                corner.color = corners[0].color;
            }
            if (oneTexel) {
                corner.u = corners[0].u;
                corner.v = corners[0].v;
            }
        }
        const Shape shape({corners[0], corners[1], corners[2]}, {0, 1, 2});
        agreement.Compare(corners, straight, Draw({shape.ToMesh()}, straight.Taken(), background), background);
    }
    EXPECT(agreement.checked > 100000);
    EXPECT(agreement.inside > 1000 && agreement.inside < agreement.checked - 1000);
    EXPECT_NEAR(agreement.worst, 0.0, 2.0);
}

TEST(RasterizerTest, "rectangles of texels are filled as a gpu fills them") {
    Random random{1992};
    const auto byte = [&] { return static_cast<uint8_t>(random.Below(256)); };
    const auto eighths = [&](uint32_t most) { return static_cast<float>(random.Below(most * 8 + 1)) / 8.0f; };
    const auto whole = [&](uint32_t most) { return static_cast<float>(random.Below(most + 1)); };
    Straight picture{8, 8, {}}, glyphs{8, 8, {}};
    for (int index = 0; index < 64; index++) {
        picture.texels.push_back({byte(), byte(), byte(), byte()});
        glyphs.texels.push_back(index % 8 == 0 || index % 8 == 5 ? std::array<uint8_t, 4>{} : std::array<uint8_t, 4>{255, 255, 255, byte()});
    }

    Agreement agreement;
    for (int round = 0; round < 800; round++) {
        const Straight& straight = round % 2 == 0 ? picture : glyphs;
        const uint32_t background = round / 8 % 2 == 0 ? Pixel::Rgba(40, 80, 120, 255) : Pixel::Rgba(20, 30, 40, 100);
        const std::array<uint8_t, 4> tint{byte(), byte(), byte(), byte()};
        const ImU32 color = round / 16 % 2 == 0 ? IM_COL32_WHITE : IM_COL32(tint[0], tint[1], tint[2], tint[3]);
        // In turn: texel for texel on the grid of pixels; texel for texel anywhere; as text is,
        // twice as wide in the texture, on whole rows, anywhere along them; stretched any way,
        // mirrored too.
        const int kind = round / 2 % 4;
        const float u = whole(5), v = whole(5);
        const float left = (kind == 0 ? whole(9) : eighths(9)) - 1.0f, top = (kind == 0 || kind == 2 ? whole(9) : eighths(9)) - 1.0f;
        const float width = kind == 3 ? 0.125f + eighths(10) : kind == 2 ? 0.5f + whole(8) / 2.0f : 1.0f + whole(5);
        const float height = kind == 3 ? 0.125f + eighths(10) : 1.0f + whole(5);
        const float across = kind == 3 ? eighths(8) - u : kind == 2 ? width * 2.0f : width, down = kind == 3 ? eighths(8) - v : height;
        const float u0 = u / 8.0f, v0 = v / 8.0f, u1 = (u + across) / 8.0f, v1 = (v + down) / 8.0f;
        const std::array<Vertex, 4> corners{Vertex{left, top, u0, v0, color}, Vertex{left + width, top, u1, v0, color}, Vertex{left + width, top + height, u1, v1, color}, Vertex{left, top + height, u0, v1, color}};
        const Shape shape({corners[0], corners[1], corners[2], corners[3]}, {0, 1, 2, 0, 2, 3});
        agreement.Compare(corners, straight, Draw({shape.ToMesh()}, straight.Taken(), background), background);
    }
    EXPECT(agreement.checked > 150000);
    EXPECT(agreement.inside > 5000 && agreement.inside < agreement.checked - 5000);
    EXPECT_NEAR(agreement.worst, 0.0, 2.0);
}

TEST(RasterizerTest, "each buffer of the window takes what it has missed") {
    const Region screen{0, 0, 100, 100}, first{0, 0, 10, 10}, second{50, 50, 60, 60}, third{90, 0, 100, 5};
    const std::array<int, 10> buffer{};
    BufferHistory buffers;
    EXPECT(buffers.Missing(&buffer[0], screen, screen) == screen);
    EXPECT(buffers.Missing(&buffer[1], screen, first) == screen);
    EXPECT(buffers.Missing(&buffer[0], screen, second) == first.Unite(second));
    EXPECT(buffers.Missing(&buffer[1], screen, third) == second.Unite(third));
    EXPECT(buffers.Missing(&buffer[1], screen, first) == first);
    EXPECT(buffers.Missing(&buffer[0], screen, Region{}) == third.Unite(first));
    EXPECT(buffers.Missing(&buffer[0], screen, Region{}).IsEmpty());

    buffers.Clear();
    EXPECT(buffers.Missing(&buffer[0], screen, first) == screen);

    // A window does not have this many buffers: what is known about them is from another one.
    for (size_t index = 1; index < 8; index++) {
        EXPECT(buffers.Missing(&buffer[index], screen, second) == screen);
    }
    EXPECT(buffers.Missing(&buffer[0], screen, third) == second.Unite(third));
    EXPECT(buffers.Missing(&buffer[8], screen, first) == screen);
    EXPECT(buffers.Missing(&buffer[0], screen, first) == screen);
    EXPECT(buffers.Missing(&buffer[8], screen, second) == first.Unite(second));
}

TEST(RasterizerTest, "a window of dear imgui is drawn and only what changes is drawn again") {
    constexpr int32_t Width = 320, Height = 240;
    const Region screen{0, 0, Width, Height};
    ImGuiContext* context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(static_cast<float>(Width), static_cast<float>(Height));
    io.DeltaTime = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;

    SoftwareTextures textures;
    Damage damage;
    std::vector<uint32_t> picture(static_cast<size_t>(Width * Height), Clear);
    struct Shown {
        ImTextureStatus asked;
        std::vector<ImTextureID> repainted;
        Region damage;
    };
    const auto show = [&](const char* text) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(40.0f, 40.0f));
        ImGui::SetNextWindowSize(ImVec2(200.0f, 120.0f));
        ImGui::Begin("Window", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted(text);
        ImGui::End();
        ImGui::Render();
        const ImDrawData& frame = *ImGui::GetDrawData();
        Shown shown{io.Fonts->TexData->Status, textures.Update(frame), {}};
        const std::vector<Mesh> found = Mesh::Collect(frame, screen);
        shown.damage = damage.Update(screen, found, shown.repainted);
        Canvas canvas(picture, Width, shown.damage);
        canvas.Clear();
        canvas.DrawMeshes(found, textures);
        return shown;
    };

    Shown shown = show("one");
    EXPECT(shown.asked == ImTextureStatus_WantCreate && shown.repainted.empty() && shown.damage == screen);
    EXPECT(io.Fonts->TexData->Status == ImTextureStatus_OK && textures.Find(io.Fonts->TexData->GetTexID()) != nullptr);
    const uint32_t background = Pixel::Over(Clear, Pixel::Unpack(ImGui::GetColorU32(ImGuiCol_WindowBg)));
    EXPECT(background >> 24 != 0);
    EXPECT_EQ(At(picture, 140, 130, Width), background);
    EXPECT_EQ(At(picture, 10, 10, Width), Clear);
    EXPECT_EQ(At(picture, 300, 200, Width), Clear);
    EXPECT(Premultiplied(picture));
    const auto written = [&] {
        int marks = 0;
        for (int y = 60; y < 90; y++) {
            for (int x = 44; x < 236; x++) {
                if (At(picture, x, y, Width) != background) {
                    marks++;
                }
            }
        }
        return marks;
    };
    const int letters = written();
    EXPECT(letters > 20);

    // A window gets the grip in its corner on its second frame; from then on nothing changes.
    shown = show("one");
    EXPECT(shown.damage.GetArea() <= 32 * 32);
    shown = show("one");
    EXPECT(shown.asked == ImTextureStatus_OK && shown.repainted.empty() && shown.damage.IsEmpty());

    // Letters not seen before join the font texture where nothing was drawn from yet: only
    // the line that shows them is drawn again.
    shown = show("one, and then some: QXZ");
    EXPECT(shown.asked == ImTextureStatus_WantUpdates && shown.repainted.empty());
    EXPECT(io.Fonts->TexData->Status == ImTextureStatus_OK);
    EXPECT(!shown.damage.IsEmpty() && shown.damage.Intersect(Region{32, 32, 256, 160}) == shown.damage && shown.damage.GetArea() < 200 * 120 / 2);
    EXPECT(written() > letters);

    // What was drawn piece by piece is what the last frame looks like drawn at once, on one
    // thread or in bands.
    const std::vector<Mesh> found = Mesh::Collect(*ImGui::GetDrawData(), screen);
    std::vector<uint32_t> whole(picture.size(), Pixel::Rgba(1, 2, 3, 4)), banded(picture.size(), Pixel::Rgba(4, 3, 2, 1));
    Canvas canvas(whole, Width, screen);
    canvas.Clear();
    canvas.DrawMeshes(found, textures);
    for (Canvas band : Canvas(banded, Width, screen).Bands(7)) {
        band.Clear();
        band.DrawMeshes(found, textures);
    }
    EXPECT(whole == picture);
    EXPECT(banded == picture);
    EXPECT(Premultiplied(picture));

    // A picture of the application's own that changes in place repaints what shows it.
    ImTextureData own;
    own.Create(ImTextureFormat_RGBA32, 2, 2);
    EXPECT(!textures.Update(own));
    own.UpdateRect = ImTextureRect{0, 0, 2, 2};
    own.SetStatus(ImTextureStatus_WantUpdates);
    EXPECT(textures.Update(own));
    textures.Destroy(own);

    for (ImTextureData* texture : ImGui::GetPlatformIO().Textures) {
        textures.Destroy(*texture);
    }
    EXPECT_EQ(textures.GetSize(), size_t{0});
    EXPECT(io.Fonts->TexData->Status == ImTextureStatus_WantCreate && io.Fonts->TexData->GetTexID() == ImTextureID_Invalid);
    ImGui::DestroyContext(context);
}
