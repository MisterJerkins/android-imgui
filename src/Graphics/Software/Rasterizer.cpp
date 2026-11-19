#include "Graphics/Software/Rasterizer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

// A mesh vertex in pixels and texels, its colour laid out as a pixel's with the alpha apart.
struct Corner {
    float x;
    float y;
    float u;
    float v;
    uint32_t color;
};

constexpr uint32_t OpaqueWhite = 0xffff'ffff;
constexpr int32_t TileSide = 32;

int32_t Pixel::Integer(float value) {
    constexpr float Farthest = 1e9f;
    return static_cast<int32_t>(Pixel::Bounded(value, -Farthest, Farthest));
}

Region Mesh::ClipRegion(const ImVec4& clip, ImVec2 origin, ImVec2 scale) {
    const auto pixel = [](float value) { return Pixel::Integer(std::round(value)); };
    return {pixel((clip.x - origin.x) * scale.x), pixel((clip.y - origin.y) * scale.y), pixel((clip.z - origin.x) * scale.x), pixel((clip.w - origin.y) * scale.y)};
}

std::vector<Mesh> Mesh::Collect(const ImDrawData& frame, Region screen) {
    std::vector<Mesh> found;
    for (const ImDrawList* list : frame.CmdLists) {
        const std::span<const ImDrawVert> vertices(list->VtxBuffer.Data, static_cast<size_t>(list->VtxBuffer.Size));
        const std::span<const ImDrawIdx> indices(list->IdxBuffer.Data, static_cast<size_t>(list->IdxBuffer.Size));
        for (const ImDrawCmd& command : list->CmdBuffer) {
            if (command.UserCallback) {
                if (command.UserCallback != ImDrawCallback_ResetRenderState) {
                    command.UserCallback(list, &command);
                }
                continue;
            }
            const Region clip = Mesh::ClipRegion(command.ClipRect, frame.DisplayPos, frame.FramebufferScale).Intersect(screen);
            if (clip.IsEmpty() || command.VtxOffset >= vertices.size() || command.IdxOffset >= indices.size()) {
                continue;
            }
            const size_t count = std::min<size_t>(command.ElemCount, indices.size() - command.IdxOffset);
            found.push_back({vertices.subspan(command.VtxOffset), indices.subspan(command.IdxOffset, count), command.GetTexID(), clip, frame.DisplayPos, frame.FramebufferScale});
        }
    }
    return found;
}

uint64_t Damage::Bits(ImVec2 pair) {
    return std::bit_cast<uint32_t>(pair.x) | static_cast<uint64_t>(std::bit_cast<uint32_t>(pair.y)) << 32;
}

void Damage::Reset() {
    shown_.clear();
}

Region Damage::Update(Region screen, std::span<const Mesh> meshes, std::span<const ImTextureID> repainted) {
    screen = screen.Intersect({0, 0, std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::max()});
    const auto tiles = [](int32_t pixels) { return (pixels + TileSide - 1) / TileSide; };
    const int32_t columns = tiles(screen.right), rows = tiles(screen.bottom);
    frame_++;
    next_.assign(static_cast<size_t>(std::max(columns * rows, 0)), 0);

    std::span<const ImDrawVert> hashed;
    for (const Mesh& mesh : meshes) {
        // The commands of a draw list share its vertices, which are hashed once for all of them.
        if (mesh.vertices.data() != hashed.data() || mesh.vertices.size() != hashed.size()) {
            hashed = mesh.vertices;
            corners_.clear();
            for (const ImDrawVert& vertex : hashed) {
                static_assert(sizeof(ImDrawVert) % sizeof(uint32_t) == 0);
                std::array<uint32_t, sizeof(ImDrawVert) / sizeof(uint32_t)> words;
                std::memcpy(words.data(), &vertex, sizeof words);
                uint64_t hash = Utils::HashSeed;
                for (const uint32_t word : words) {
                    hash = Utils::HashMix(hash, word);
                }
                corners_.push_back(hash);
            }
        }
        const bool again = std::ranges::find(repainted, mesh.texture) != repainted.end();
        const uint64_t parts[] = {mesh.texture, static_cast<uint64_t>(mesh.clip.left), static_cast<uint64_t>(mesh.clip.top), static_cast<uint64_t>(mesh.clip.right), static_cast<uint64_t>(mesh.clip.bottom), Bits(mesh.origin), Bits(mesh.scale), again ? frame_ : 0};
        uint64_t salt = Utils::HashSeed;
        for (const uint64_t part : parts) {
            salt = Utils::HashMix(salt, part);
        }

        const auto corner = [&](ImDrawIdx index) {
            const ImVec2 at = mesh.vertices[index].pos;
            return ImVec2((at.x - mesh.origin.x) * mesh.scale.x, (at.y - mesh.origin.y) * mesh.scale.y);
        };
        for (size_t first = 0; first + 3 <= mesh.indices.size(); first += 3) {
            const ImDrawIdx* triangle = &mesh.indices[first];
            if (std::max({triangle[0], triangle[1], triangle[2]}) >= mesh.vertices.size()) {
                continue;
            }
            const ImVec2 a = corner(triangle[0]), b = corner(triangle[1]), c = corner(triangle[2]);
            const float left = std::min(a.x, std::min(b.x, c.x)), right = std::max(a.x, std::max(b.x, c.x));
            const float top = std::min(a.y, std::min(b.y, c.y)), bottom = std::max(a.y, std::max(b.y, c.y));
            const Region reach = Region{Pixel::Integer(std::floor(left)), Pixel::Integer(std::floor(top)), Pixel::Integer(std::ceil(right)), Pixel::Integer(std::ceil(bottom))}.Intersect(mesh.clip).Intersect(screen);
            if (reach.IsEmpty()) {
                continue;
            }
            uint64_t hash = salt;
            for (size_t index = 0; index < 3; index++) {
                hash = Utils::HashMix(hash, corners_[triangle[index]]);
            }
            for (int32_t row = reach.top / TileSide; row <= (reach.bottom - 1) / TileSide; row++) {
                for (int32_t column = reach.left / TileSide; column <= (reach.right - 1) / TileSide; column++) {
                    uint64_t& tile = next_[static_cast<size_t>(row * columns + column)];
                    tile = Utils::HashMix(tile, hash);
                }
            }
        }
    }

    Region changed;
    if (columns != columns_ || shown_.size() != next_.size()) {
        changed = screen;
    } else {
        for (size_t index = 0; index < next_.size(); index++) {
            if (next_[index] == shown_[index]) {
                continue;
            }
            const int32_t column = static_cast<int32_t>(index) % columns, row = static_cast<int32_t>(index) / columns;
            changed = changed.Unite({column * TileSide, row * TileSide, (column + 1) * TileSide, (row + 1) * TileSide});
        }
    }
    std::swap(shown_, next_);
    columns_ = columns;
    return changed.Intersect(screen);
}

void SoftwareTexture::Load(const ImTextureData& source) {
    width_ = static_cast<size_t>(std::max(source.Width, 0));
    height_ = static_cast<size_t>(std::max(source.Height, 0));
    pixels_.assign(width_ * height_, 0);
    Take(source, 0, 0, width_, height_);
}

void SoftwareTexture::Patch(const ImTextureData& source, ImTextureRect part) {
    Take(source, part.x, part.y, part.w, part.h);
}

void SoftwareTexture::Take(const ImTextureData& source, size_t left, size_t top, size_t width, size_t height) {
    const size_t across = std::min(width_, static_cast<size_t>(std::max(source.Width, 0))), down = std::min(height_, static_cast<size_t>(std::max(source.Height, 0)));
    if (!source.Pixels || left >= across || top >= down) {
        return;
    }
    width = std::min(width, across - left);
    height = std::min(height, down - top);
    const size_t step = static_cast<size_t>(source.BytesPerPixel), pitch = static_cast<size_t>(source.GetPitch());
    for (size_t row = top; row < top + height; row++) {
        const uint8_t* from = source.Pixels + row * pitch + left * step;
        uint32_t* to = pixels_.data() + row * width_ + left;
        if (source.Format == ImTextureFormat_Alpha8) {
            for (size_t column = 0; column < width; column++) {
                to[column] = Pixel::Rgba(255, 255, 255, from[column]);
            }
        } else {
            for (size_t column = 0; column < width; column++, from += 4) {
                to[column] = Pixel::Rgba(from[0], from[1], from[2], from[3]);
            }
        }
    }
}

uint32_t SoftwareTexture::Texel(int32_t x, int32_t y) const {
    if (pixels_.empty()) {
        return 0;
    }
    const auto inside = [](int32_t value, size_t size) { return static_cast<size_t>(std::clamp(value, 0, static_cast<int32_t>(size) - 1)); };
    return pixels_[inside(y, height_) * width_ + inside(x, width_)];
}

SoftwareTexture::Level SoftwareTexture::LevelAt(float v) const {
    const float y = Pixel::Bounded(v - 0.5f, -1.0f, static_cast<float>(height_)), top = std::floor(y);
    const auto row = [&](int32_t index) { return pixels_.data() + static_cast<size_t>(std::clamp(index, 0, static_cast<int32_t>(height_) - 1)) * width_; };
    return {row(static_cast<int32_t>(top)), row(static_cast<int32_t>(top) + 1), static_cast<uint32_t>((y - top) * 256.0f)};
}

uint32_t SoftwareTexture::Between(const Level& at, size_t first, size_t second, uint32_t across) {
    constexpr uint32_t Pair = 0x00ff'00ff, Half = 0x0080'0080;
    const uint32_t a = at.upper[first], b = at.upper[second];
    // Glyphs lie on whole rows of texels, whatever their width is stretched to: most of what
    // is read from a texture needs one row only, and takes two channels at a time.
    if (at.down == 0) {
        const uint32_t low = (a & Pair) * (256 - across) + (b & Pair) * across + Half, high = (a >> 8 & Pair) * (256 - across) + (b >> 8 & Pair) * across + Half;
        return (low >> 8 & Pair) | (high & ~Pair);
    }
    const uint32_t c = at.lower[first], d = at.lower[second];
    uint32_t color = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const uint32_t above = (a >> shift & 0xff) * (256 - across) + (b >> shift & 0xff) * across;
        const uint32_t below = (c >> shift & 0xff) * (256 - across) + (d >> shift & 0xff) * across;
        color |= (above * (256 - at.down) + below * at.down + (1 << 15)) >> 16 << shift;
    }
    return color;
}

uint32_t SoftwareTexture::Sample(const Level& at, float u) const {
    const float x = Pixel::Bounded(u - 0.5f, -1.0f, static_cast<float>(width_)), left = std::floor(x);
    const auto column = [&](int32_t index) { return static_cast<size_t>(std::clamp(index, 0, static_cast<int32_t>(width_) - 1)); };
    return Between(at, column(static_cast<int32_t>(left)), column(static_cast<int32_t>(left) + 1), static_cast<uint32_t>((x - left) * 256.0f));
}

void SoftwareTexture::Read(const Level& at, float u, float across, std::span<uint32_t> colors) const {
    // Positions are counted in 2^-32 of a texel: a step added a thousand times over is still
    // where a multiplication would put it. They wrap around where no texture reaches.
    constexpr float Unit = 4294967296.0f, Farthest = 1073741824.0f;
    uint64_t position = static_cast<uint64_t>(static_cast<int64_t>(Pixel::Bounded(u - 0.5f, -Farthest, Farthest) * Unit));
    const uint64_t step = static_cast<uint64_t>(static_cast<int64_t>(Pixel::Bounded(across, -Farthest, Farthest) * Unit));
    const int64_t last = static_cast<int64_t>(width_) - 1;
    for (uint32_t& color : colors) {
        const int64_t left = static_cast<int64_t>(position) >> 32;
        color = Between(at, static_cast<size_t>(std::clamp<int64_t>(left, 0, last)), static_cast<size_t>(std::clamp<int64_t>(left + 1, 0, last)), static_cast<uint32_t>(position >> 24) & 0xff);
        position += step;
    }
}

std::span<const uint32_t> SoftwareTexture::Run(float u, float v, size_t length) const {
    const auto centred = [](float at) { return std::fabs(at - 0.5f - std::round(at - 0.5f)) < 1.0f / 256.0f; };
    if (!(u >= 0.0f && v >= 0.0f && u < static_cast<float>(width_) && v < static_cast<float>(height_) && centred(u) && centred(v))) {
        return {};
    }
    const size_t column = static_cast<size_t>(u), row = static_cast<size_t>(v);
    if (column + length > width_) {
        return {};
    }
    return {pixels_.data() + row * width_ + column, length};
}

std::vector<ImTextureID> SoftwareTextures::Update(const ImDrawData& frame) {
    std::vector<ImTextureID> repainted;
    if (!frame.Textures) {
        return repainted;
    }
    for (ImTextureData* texture : *frame.Textures) {
        if (texture->Status != ImTextureStatus_OK && Update(*texture)) {
            repainted.push_back(texture->GetTexID());
        }
    }
    return repainted;
}

bool SoftwareTextures::Update(ImTextureData& texture) {
    const auto known = textures_.find(texture.GetTexID());
    if (texture.Status == ImTextureStatus_WantCreate || (texture.Status == ImTextureStatus_WantUpdates && known == textures_.end())) {
        const ImTextureID id = next_++;
        textures_[id].Load(texture);
        texture.SetTexID(id);
        texture.SetStatus(ImTextureStatus_OK);
        // Nothing was drawn under this identifier before: every tile it reaches is new by that alone.
        return false;
    }
    if (texture.Status == ImTextureStatus_WantUpdates) {
        for (const ImTextureRect& part : texture.Updates) {
            known->second.Patch(texture, part);
        }
        if (texture.Updates.empty()) {
            known->second.Patch(texture, texture.UpdateRect);
        }
        texture.SetStatus(ImTextureStatus_OK);
        // New glyphs join the font texture in place; any other texture change repaints what shows it.
        return !(ImGui::GetCurrentContext() && ImGui::GetIO().Fonts->TexData == &texture);
    }
    if (texture.Status == ImTextureStatus_WantDestroy && texture.UnusedFrames > 0) {
        Destroy(texture);
    }
    return false;
}

void SoftwareTextures::Destroy(ImTextureData& texture) {
    textures_.erase(texture.GetTexID());
    texture.SetTexID(ImTextureID_Invalid);
    texture.SetStatus(ImTextureStatus_Destroyed);
}

const SoftwareTexture* SoftwareTextures::Find(ImTextureID id) const {
    const auto found = textures_.find(id);
    return found == textures_.end() ? nullptr : &found->second;
}

Canvas::Canvas(std::span<uint32_t> pixels, size_t stride, Region area) : pixels_(pixels), stride_(stride) {
    const size_t rows = stride == 0 ? 0 : pixels.size() / stride;
    area_ = area.Intersect({0, 0, static_cast<int32_t>(stride), static_cast<int32_t>(rows)});
    if (area_.IsEmpty()) {
        area_ = {};
    }
}

std::vector<Canvas> Canvas::Bands(size_t count) const {
    const size_t rows = static_cast<size_t>(area_.bottom - area_.top), parts = std::max<size_t>(count, 1);
    const int32_t height = static_cast<int32_t>(std::max<size_t>((rows + parts - 1) / parts, 1));
    std::vector<Canvas> cut;
    for (int32_t top = area_.top; top < area_.bottom; top += height) {
        Canvas band = *this;
        band.area_.top = top;
        band.area_.bottom = std::min(top + height, area_.bottom);
        cut.push_back(band);
    }
    return cut;
}

void Canvas::Clear() {
    for (int32_t y = area_.top; y < area_.bottom; y++) {
        std::ranges::fill(Row(y, area_.left, area_.right), 0u);
    }
}

void Canvas::DrawMeshes(std::span<const Mesh> meshes, const SoftwareTextures& textures) {
    for (const Mesh& part : meshes) {
        if (const SoftwareTexture* texture = textures.Find(part.texture)) {
            DrawMesh(part, *texture);
        }
    }
}

void Canvas::DrawMesh(const Mesh& part, const SoftwareTexture& texture) {
    const Region clip = part.clip.Intersect(area_);
    if (clip.IsEmpty() || texture.IsEmpty()) {
        return;
    }
    const float width = static_cast<float>(texture.GetWidth()), height = static_cast<float>(texture.GetHeight());
    const auto corner = [&](ImDrawIdx index) {
        const ImDrawVert& vertex = part.vertices[index];
        return Corner{OnGrid((vertex.pos.x - part.origin.x) * part.scale.x), OnGrid((vertex.pos.y - part.origin.y) * part.scale.y), vertex.uv.x * width, vertex.uv.y * height, Pixel::Unpack(vertex.col)};
    };
    const std::span<const ImDrawIdx> indices = part.indices;
    const size_t count = part.vertices.size();
    for (size_t first = 0; first + 3 <= indices.size();) {
        const ImDrawIdx* index = &indices[first];
        if (index[0] >= count || index[1] >= count || index[2] >= count) {
            return;
        }
        const Corner a = corner(index[0]), b = corner(index[1]), c = corner(index[2]);
        // Rectangles come as two triangles over four corners; those need no edge walking.
        if (first + 6 <= indices.size() && index[3] == index[0] && index[4] == index[2] && index[5] < count && Rectangle(a, b, corner(index[5]), c, clip, texture)) {
            first += 6;
            continue;
        }
        Triangle(a, b, c, clip, texture);
        first += 3;
    }
}

int32_t Canvas::Snap(float edge) {
    return Pixel::Integer(std::ceil(edge - 0.5f));
}

float Canvas::OnGrid(float position) {
    constexpr float Steps = 256.0f;
    return std::round(position * Steps) / Steps;
}

void Canvas::Fill(std::span<uint32_t> line, uint32_t color) {
    if (color >> 24 == 255) {
        std::ranges::fill(line, color);
    } else if (color >> 24 != 0) {
        for (uint32_t& pixel : line) {
            pixel = Pixel::Over(pixel, color);
        }
    }
}

void Canvas::Lay(std::span<uint32_t> line, const uint32_t* texels, uint32_t color) {
    for (size_t index = 0; index < line.size(); index++) {
        line[index] = Pixel::Over(line[index], Pixel::Modulate(texels[index], color));
    }
}

uint32_t Canvas::Shaded(const std::array<float, 4>& channels) {
    const auto byte = [](float value) { return static_cast<uint32_t>(Pixel::Bounded(value, 0.0f, 255.0f) + 0.5f); };
    return Pixel::Rgba(byte(channels[0]), byte(channels[1]), byte(channels[2]), byte(channels[3]));
}

std::span<uint32_t> Canvas::Row(int32_t y, int32_t left, int32_t right) const {
    return pixels_.subspan(static_cast<size_t>(y) * stride_ + static_cast<size_t>(left), static_cast<size_t>(right - left));
}

bool Canvas::Rectangle(const Corner& a, const Corner& b, const Corner& c, const Corner& d, Region clip, const SoftwareTexture& texture) {
    const bool upright = a.y == b.y && c.y == d.y && a.x == c.x && b.x == d.x && a.x < b.x && a.y < c.y;
    const bool mapped = a.v == b.v && c.v == d.v && a.u == c.u && b.u == d.u;
    if (!(upright && mapped && a.color == b.color && b.color == c.color && c.color == d.color)) {
        return false;
    }
    const Region inside = Region{Snap(a.x), Snap(a.y), Snap(d.x), Snap(d.y)}.Intersect(clip);
    if (inside.IsEmpty()) {
        return true;
    }
    const float across = (d.u - a.u) / (d.x - a.x), down = (d.v - a.v) / (d.y - a.y);
    if (across == 0.0f && down == 0.0f) {
        const uint32_t plain = Pixel::Modulate(texture.Sample(a.u, a.v), a.color);
        for (int32_t y = inside.top; y < inside.bottom; y++) {
            Fill(Row(y, inside.left, inside.right), plain);
        }
        return true;
    }
    const bool whole = std::fabs(across - 1.0f) < 1e-4f && std::fabs(down - 1.0f) < 1e-4f;
    for (int32_t y = inside.top; y < inside.bottom; y++) {
        const float u = a.u + (static_cast<float>(inside.left) + 0.5f - a.x) * across, v = a.v + (static_cast<float>(y) + 0.5f - a.y) * down;
        const std::span<uint32_t> line = Row(y, inside.left, inside.right);
        if (const std::span<const uint32_t> texels = whole ? texture.Run(u, v, line.size()) : std::span<const uint32_t>(); !texels.empty()) {
            Lay(line, texels.data(), a.color);
            continue;
        }
        // What is stretched is read a stretch at a time, and laid down like a run of texels.
        const SoftwareTexture::Level level = texture.LevelAt(v);
        std::array<uint32_t, 256> stretch;
        for (size_t done = 0; done < line.size(); done += stretch.size()) {
            const std::span<uint32_t> part = line.subspan(done, std::min(stretch.size(), line.size() - done));
            texture.Read(level, u + static_cast<float>(done) * across, across, std::span(stretch).first(part.size()));
            Lay(part, stretch.data(), a.color);
        }
    }
    return true;
}

void Canvas::Triangle(const Corner& a, const Corner& b, const Corner& c, Region clip, const SoftwareTexture& texture) {
    const int32_t top = std::max(Snap(std::min(a.y, std::min(b.y, c.y))), clip.top);
    const int32_t bottom = std::min(Snap(std::max(a.y, std::max(b.y, c.y))), clip.bottom);
    const float abx = b.x - a.x, aby = b.y - a.y, acx = c.x - a.x, acy = c.y - a.y;
    const float cross = abx * acy - aby * acx;
    if (top >= bottom || cross == 0.0f || !std::isfinite(cross)) {
        return;
    }

    // How fast a value given at the three corners changes per pixel across and down.
    struct Slope {
        float across;
        float down;
    };
    const auto slope = [&](float atA, float atB, float atC) { return Slope{((atB - atA) * acy - (atC - atA) * aby) / cross, ((atC - atA) * abx - (atB - atA) * acx) / cross}; };
    const bool plain = a.color == b.color && b.color == c.color;
    const bool flat = a.u == b.u && b.u == c.u && a.v == b.v && b.v == c.v;
    const uint32_t texel = flat ? texture.Sample(a.u, a.v) : OpaqueWhite;
    const uint32_t filling = Pixel::Modulate(texel, a.color);

    std::array<float, 4> channels{};
    std::array<Slope, 4> shades{};
    if (!plain) {
        for (size_t index = 0; index < 4; index++) {
            const auto channel = [&](uint32_t color) { return static_cast<float>(color >> (index * 8) & 0xff); };
            channels[index] = channel(a.color);
            shades[index] = slope(channels[index], channel(b.color), channel(c.color));
        }
    }
    const Slope alongU = flat ? Slope{} : slope(a.u, b.u, c.u), alongV = flat ? Slope{} : slope(a.v, b.v, c.v);

    // An edge is always walked from its upper end, so both triangles on it agree on where it runs.
    struct Edge {
        float top;
        float bottom;
        float x;
        float lean;
    };
    const auto edge = [](const Corner& from, const Corner& to) {
        const bool downwards = from.y < to.y || (from.y == to.y && from.x <= to.x);
        const Corner& upper = downwards ? from : to;
        const Corner& lower = downwards ? to : from;
        return Edge{upper.y, lower.y, upper.x, lower.y > upper.y ? (lower.x - upper.x) / (lower.y - upper.y) : 0.0f};
    };
    const std::array<Edge, 3> edges{edge(a, b), edge(b, c), edge(c, a)};

    for (int32_t y = top; y < bottom; y++) {
        const float centre = static_cast<float>(y) + 0.5f;
        float left = std::numeric_limits<float>::infinity(), right = -std::numeric_limits<float>::infinity();
        for (const Edge& side : edges) {
            if (centre >= side.top && centre < side.bottom) {
                const float x = side.x + (centre - side.top) * side.lean;
                left = std::min(left, x);
                right = std::max(right, x);
            }
        }
        const int32_t start = std::max(Snap(left), clip.left), end = std::min(Snap(right), clip.right);
        if (start >= end) {
            continue;
        }
        const std::span<uint32_t> line = Row(y, start, end);
        if (plain && flat) {
            Fill(line, filling);
            continue;
        }
        const float dx = static_cast<float>(start) + 0.5f - a.x, dy = centre - a.y;
        std::array<float, 4> shade{};
        if (!plain) {
            for (size_t index = 0; index < 4; index++) {
                shade[index] = channels[index] + dx * shades[index].across + dy * shades[index].down;
            }
        }
        float u = a.u + dx * alongU.across + dy * alongU.down, v = a.v + dx * alongV.across + dy * alongV.down;
        for (uint32_t& pixel : line) {
            uint32_t color = a.color, source = texel;
            if (!plain) {
                color = Shaded(shade);
                for (size_t index = 0; index < 4; index++) {
                    shade[index] += shades[index].across;
                }
            }
            if (!flat) {
                source = texture.Sample(u, v);
                u += alongU.across;
                v += alongV.across;
            }
            pixel = Pixel::Over(pixel, source == OpaqueWhite ? color : Pixel::Modulate(source, color));
        }
    }
}

Region BufferHistory::Missing(const void* address, Region screen, Region damage) {
    const auto known = std::ranges::find(behind_, address, [](const auto& buffer) { return buffer.first; });
    const Region lacking = known == behind_.end() ? screen : known->second.Unite(damage);
    for (auto& [buffer, behind] : behind_) {
        behind = behind.Unite(damage);
    }
    if (known != behind_.end()) {
        known->second = {};
    } else {
        // A window has a few buffers at most; more of them than eight means the old ones are gone.
        if (behind_.size() == 8) {
            behind_.clear();
        }
        behind_.emplace_back(address, Region{});
    }
    return lacking;
}
