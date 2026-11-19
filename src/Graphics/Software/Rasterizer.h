#pragma once

#include "Window/ANativeWindowCreator.h"

#include "imgui.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

// The part of the CPU backend that needs no window: what a frame changes on the screen, the
// textures it draws with, and its triangles turned into pixels. A pixel is what the window's
// buffer holds: a premultiplied colour with red in the lowest byte and alpha in the highest.

// The arithmetic of pixels.
class Pixel {
public:
    static constexpr uint32_t Rgba(uint32_t red, uint32_t green, uint32_t blue, uint32_t alpha) {
        return red | green << 8 | blue << 16 | alpha << 24;
    }

    static constexpr uint32_t Div255(uint32_t value) {
        value += 128;
        return (value + (value >> 8)) >> 8;
    }

    static constexpr uint32_t Modulate(uint32_t a, uint32_t b) {
        return Pixel::Rgba(Pixel::Div255((a & 0xff) * (b & 0xff)), Pixel::Div255((a >> 8 & 0xff) * (b >> 8 & 0xff)), Pixel::Div255((a >> 16 & 0xff) * (b >> 16 & 0xff)), Pixel::Div255((a >> 24) * (b >> 24)));
    }

    // A colour of Dear ImGui's with its channels where a pixel has them, its alpha still apart.
    static constexpr uint32_t Unpack(ImU32 color) {
        return Pixel::Rgba(color >> IM_COL32_R_SHIFT & 0xff, color >> IM_COL32_G_SHIFT & 0xff, color >> IM_COL32_B_SHIFT & 0xff, color >> IM_COL32_A_SHIFT & 0xff);
    }

    // Lays a colour over a pixel. Dear ImGui keeps a colour's alpha apart from it, in vertices
    // and in textures; the window takes colours multiplied by their alpha, which is how the
    // system composes its layers. A GPU multiplies as it blends, after shading and filtering,
    // and rounds once: so it is done here, two channels at a time, and all backends draw the
    // same picture.
    static constexpr uint32_t Over(uint32_t pixel, uint32_t color) {
        constexpr uint32_t Pair = 0x00ff'00ff, Half = 0x0080'0080;
        const uint32_t alpha = color >> 24, keep = 255 - alpha;
        const uint32_t low = (color & Pair) * alpha + (pixel & Pair) * keep + Half;
        const uint32_t high = ((color >> 8 & 0xff) | 0x00ff'0000) * alpha + (pixel >> 8 & Pair) * keep + Half;
        return ((low + (low >> 8 & Pair)) >> 8 & Pair) | ((high + (high >> 8 & Pair)) & ~Pair);
    }

    // A float held within bounds; what is no number becomes the lower one.
    static constexpr float Bounded(float value, float low, float high) {
        return value > low ? (value < high ? value : high) : low;
    }

    // The integer a float holds. A float from beyond what the integer takes must not be
    // converted at all, so this stops earlier, far outside of any surface.
    static int32_t Integer(float value);
};

// The triangles of one draw command. A vertex becomes a pixel position by taking `origin`
// off and multiplying by `scale`.
struct Mesh {
    std::span<const ImDrawVert> vertices;
    std::span<const ImDrawIdx> indices;
    ImTextureID texture = ImTextureID_Invalid;
    Region clip;
    ImVec2 origin{0.0f, 0.0f};
    ImVec2 scale{1.0f, 1.0f};

    // A command's clip rectangle in pixels of the frame, whose positions have `origin` taken
    // off and are multiplied by `scale`.
    static Region ClipRegion(const ImVec4& clip, ImVec2 origin, ImVec2 scale);
    // The meshes of a frame in drawing order, clipped to `screen`. A command that is a callback
    // draws nothing: it is called here, in its turn, unless all it asks for is the render
    // state a GPU would have.
    static std::vector<Mesh> Collect(const ImDrawData& frame, Region screen);
};

// Remembers what every tile of the screen showed, to find what a new frame changes. A tile
// is known by a hash of the triangles that reach into it, in drawing order.
class Damage {
public:
    // Forgets the picture, so that the next frame counts as new all over.
    void Reset();

    // Takes in a frame and returns the smallest region holding every tile it changes.
    // Meshes drawn with a texture named in `repainted` count as changed even where their
    // shape is the same.
    Region Update(Region screen, std::span<const Mesh> meshes, std::span<const ImTextureID> repainted);

private:
    static uint64_t Bits(ImVec2 pair);

    int32_t columns_ = 0;
    std::vector<uint64_t> shown_;
    std::vector<uint64_t> next_;
    std::vector<uint64_t> corners_;
    uint64_t frame_ = 0;
};

// A texture's pixels as Dear ImGui hands them over, their alpha apart. Where they are
// stretched they are filtered between four texels, each channel by itself, and they end at
// their edges: what a GPU is told to do with them, and what Dear ImGui's glyphs are made
// for, the small ones being twice as wide in the font texture as they are on the screen.
class SoftwareTexture {
public:
    // Takes every pixel of a texture of Dear ImGui's, in either of its formats.
    void Load(const ImTextureData& source);
    // Takes the pixels of one rectangle of it again.
    void Patch(const ImTextureData& source, ImTextureRect part);

    size_t GetWidth() const {
        return width_;
    }
    size_t GetHeight() const {
        return height_;
    }
    bool IsEmpty() const {
        return pixels_.empty();
    }
    // The texel nearest to a position that may lie outside.
    uint32_t Texel(int32_t x, int32_t y) const;

private:
    friend class Canvas;

    // A height at which the texture is read: the rows of texels above and below it, and how
    // far down between them it lies, in 256ths.
    struct Level {
        const uint32_t* upper;
        const uint32_t* lower;
        uint32_t down;
    };

    void Take(const ImTextureData& source, size_t left, size_t top, size_t width, size_t height);
    Level LevelAt(float v) const;
    // The colour `across` 256ths of the way from one column of a level to another.
    static uint32_t Between(const Level& at, size_t first, size_t second, uint32_t across);
    uint32_t Sample(const Level& at, float u) const;
    // The colour at a position measured in texels.
    uint32_t Sample(float u, float v) const {
        return Sample(LevelAt(v), u);
    }
    // The colours along a level, the first at `u` and each `across` texels after the last.
    void Read(const Level& at, float u, float across, std::span<uint32_t> colors) const;
    // A run of texels starting at the centre of one, when all of it lies inside the texture.
    std::span<const uint32_t> Run(float u, float v, size_t length) const;

    size_t width_ = 0;
    size_t height_ = 0;
    std::vector<uint32_t> pixels_;
};

// The textures Dear ImGui has asked for, each under the identifier its draw commands carry.
class SoftwareTextures {
public:
    // Creates, updates and destroys what the textures of a frame ask for, and returns those
    // whose pixels have changed under meshes that may be on the screen already.
    std::vector<ImTextureID> Update(const ImDrawData& frame);

    // Does what one texture asks for, as `ImGui_ImplOpenGL3_UpdateTexture` does. `true` when
    // pixels that may be on the screen already have changed.
    bool Update(ImTextureData& texture);

    // Lets go of a texture whatever it asks for, as a backend does when it ends: whoever
    // draws next is asked to create it again.
    void Destroy(ImTextureData& texture);

    const SoftwareTexture* Find(ImTextureID id) const;
    size_t GetSize() const {
        return textures_.size();
    }

private:
    std::unordered_map<ImTextureID, SoftwareTexture> textures_;
    ImTextureID next_ = 1;
};

struct Corner;

// Rows of premultiplied RGBA to draw on, and the part of them that may change.
class Canvas {
public:
    // `pixels` holds rows of `stride` pixels from the top of the surface; what of `area`
    // lies outside of them is left out.
    Canvas(std::span<uint32_t> pixels, size_t stride, Region area);

    Region GetArea() const {
        return area_;
    }

    // Cuts the canvas into horizontal bands that can be drawn at the same time.
    std::vector<Canvas> Bands(size_t count) const;

    void Clear();
    void DrawMesh(const Mesh& part, const SoftwareTexture& texture);
    // Draws the meshes whose textures are there, in their order.
    void DrawMeshes(std::span<const Mesh> meshes, const SoftwareTextures& textures);

private:
    // The column or row whose centre is the first one at or past `edge`: the pixels a shape
    // covers are those whose centres lie inside it, as on a GPU.
    static int32_t Snap(float edge);
    // A position as a GPU keeps it, on a grid of 1/256 of a pixel. Dear ImGui puts glyphs down
    // at any fraction of a pixel, and an edge a hair off a pixel's centre is on it once it is
    // on the grid: the pixel then goes to the same side as on a GPU.
    static float OnGrid(float position);
    static void Fill(std::span<uint32_t> line, uint32_t color);
    // Lays texels tinted with a colour over a row of pixels.
    static void Lay(std::span<uint32_t> line, const uint32_t* texels, uint32_t color);
    // A colour shaded across a triangle at one pixel. Its channels are held within a byte: a
    // sliver of a triangle can make anything of its slopes.
    static uint32_t Shaded(const std::array<float, 4>& channels);

    std::span<uint32_t> Row(int32_t y, int32_t left, int32_t right) const;
    // Draws an upright rectangle given as top-left, top-right, bottom-left and bottom-right
    // corners. Returns `false` when the corners are anything else.
    bool Rectangle(const Corner& a, const Corner& b, const Corner& c, const Corner& d, Region clip, const SoftwareTexture& texture);
    void Triangle(const Corner& a, const Corner& b, const Corner& c, Region clip, const SoftwareTexture& texture);

    std::span<uint32_t> pixels_;
    size_t stride_;
    Region area_;
};

// The window's buffers by address, and where each one is behind the picture.
class BufferHistory {
public:
    void Clear() {
        behind_.clear();
    }

    // What the buffer at `address` has to take from the picture once `damage` is drawn anew
    // there: all of `screen` for a buffer not seen before. That buffer then counts as up to
    // date, and every other one as behind by `damage` as well.
    Region Missing(const void* address, Region screen, Region damage);

private:
    std::vector<std::pair<const void*, Region>> behind_;
};
