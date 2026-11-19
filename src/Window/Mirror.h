#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// Copies of the overlay for the displays that screen recordings read from.
//
// From Android 14 a recording takes its picture from the window manager's own layers and so
// misses a layer made beside them. Each such display has a layer stack of its own; a mirror of
// the overlay is put on every one of them, scaled the way the system scales the screen there.

// A display as `dumpsys display` lists it: its layer stack and the size of the space it shows.
struct ShownDisplay {
    uint32_t stack = 0;
    std::array<float, 2> size{};

    bool operator==(const ShownDisplay&) const = default;
};

// How a picture sits on a display: its scale and its offset.
struct MirrorPlacement {
    float scale = 1.0f;
    std::array<float, 2> offset{};

    bool operator==(const MirrorPlacement&) const = default;
};

class Mirror {
public:
    // Reads the display devices out of `dumpsys display`. Displays that are off have no layer stack.
    static std::vector<ShownDisplay> ParseDisplays(std::string_view dump);
    // Fits a `source`-sized picture into a `target`-sized display, whole and centred.
    static MirrorPlacement Fit(std::array<float, 2> source, std::array<float, 2> target);

private:
    // What follows `name` on the first line of `device` that starts with it.
    static std::optional<std::string_view> Field(std::string_view device, std::string_view name);
    // The size of `Rect(left, top - right, bottom)`, given what follows its opening bracket.
    static std::optional<std::array<float, 2>> RectSize(std::string_view rect);
};
