#include "Input/Touch/TouchDevice.h"

#include "Utils/Utils.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <limits>
#include <tuple>
#include <utility>

void RawEvent::Decode(std::span<const uint8_t> bytes, size_t timeSize, std::vector<RawEvent>& out) {
    const size_t record = timeSize + sizeof(uint16_t) * 2 + sizeof(int32_t);
    for (size_t start = 0; start + record <= bytes.size(); start += record) {
        const uint8_t* field = bytes.data() + start + timeSize;
        RawEvent event;
        std::memcpy(&event.kind, field, sizeof(event.kind));
        std::memcpy(&event.code, field + 2, sizeof(event.code));
        std::memcpy(&event.value, field + 4, sizeof(event.value));
        out.push_back(event);
    }
}

void RawEvent::Encode(size_t timeSize, std::vector<uint8_t>& bytes) const {
    const size_t field = bytes.size() + timeSize;
    bytes.resize(field + sizeof(kind) + sizeof(code) + sizeof(value), 0);
    std::memcpy(bytes.data() + field, &kind, sizeof(kind));
    std::memcpy(bytes.data() + field + 2, &code, sizeof(code));
    std::memcpy(bytes.data() + field + 4, &value, sizeof(value));
}

void AxisState::Rewind(const AxisState& earlier, std::vector<RawEvent>& out) const {
    // Each value is approached from afar, which gets it past the kernel's jitter filter intact.
    constexpr int32_t Afar = 1 << 12;
    const auto slotted = [](uint16_t code) { return code >= AbsMtFirst && code <= AbsMtLast; };
    const auto positional = [&slotted](uint16_t code) { return slotted(code) && code != AbsMtTrackingId; };
    const auto restore = [&out](uint16_t code, int32_t value) {
        // The sum wraps around where a plain one would overflow, which is undefined.
        out.push_back({EvAbs, code, static_cast<int32_t>(static_cast<uint32_t>(value) + static_cast<uint32_t>(Afar))});
        out.push_back({EvAbs, code, value});
    };
    const auto address = [&out](int32_t number) { out.push_back({EvAbs, AbsMtSlot, number}); };
    const size_t start = out.size();

    size_t slots = 0;
    for (const auto& [code, held] : values) {
        if (positional(code)) {
            slots = std::max(slots, held.size());
        }
    }
    for (size_t number = 0; number < slots; number++) {
        bool addressed = false;
        for (const auto& [code, held] : values) {
            if (!positional(code)) {
                continue;
            }
            const std::span<const int32_t> then = earlier.Column(code);
            if (number >= held.size() || number >= then.size()) {
                continue;
            }
            if (held[number] != then[number]) {
                if (!std::exchange(addressed, true)) {
                    address(static_cast<int32_t>(number));
                }
                restore(code, then[number]);
            }
        }
    }
    // A slot is announced only together with a change in it, and the reader expects the old one.
    if (earlier.slot >= 0 && (out.size() > start || slot != earlier.slot)) {
        const size_t last = static_cast<size_t>(earlier.slot);
        for (const auto& [code, held] : values) {
            if (!positional(code)) {
                continue;
            }
            const std::span<const int32_t> then = earlier.Column(code);
            if (last >= then.size()) {
                continue;
            }
            address(earlier.slot);
            restore(code, then[last]);
            break;
        }
    }
    for (const auto& [code, held] : values) {
        if (slotted(code)) {
            continue;
        }
        const std::span<const int32_t> then = earlier.Column(code);
        if (held.size() == 1 && then.size() == 1 && held[0] != then[0]) {
            restore(code, then[0]);
        }
    }
    if (out.size() > start) {
        out.push_back({EvSyn, SynReport, 0});
    }
}

std::span<const int32_t> AxisState::Column(uint16_t code) const {
    for (const auto& [other, held] : values) {
        if (other == code) {
            return held;
        }
    }
    return {};
}

int32_t Axis::Span() const {
    const int64_t wide = static_cast<int64_t>(max) - static_cast<int64_t>(min);
    return static_cast<int32_t>(std::clamp<int64_t>(wide, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()));
}

float Axis::Normalize(int32_t value) const {
    const float size = static_cast<float>(std::max<int64_t>(static_cast<int64_t>(max) - static_cast<int64_t>(min) + 1, 1));
    return std::clamp(static_cast<float>(static_cast<int64_t>(value) - static_cast<int64_t>(min)) / size, 0.0f, 1.0f);
}

bool Capabilities::HasProperty(uint32_t property) const {
    return property < 32 && (properties & (uint32_t{1} << property)) != 0;
}

bool Capabilities::HasRelative(uint16_t code) const {
    return code < 32 && (relative & (uint32_t{1} << code)) != 0;
}

bool Capabilities::HasKey(uint16_t code) const {
    const size_t byte = code / 8;
    return byte < keys.size() && (keys[byte] & (1u << (code % 8))) != 0;
}

bool Capabilities::HasKeyIn(uint16_t begin, uint16_t end) const {
    for (uint32_t code = begin; code < end; code++) {
        if (HasKey(static_cast<uint16_t>(code))) {
            return true;
        }
    }
    return false;
}

bool Capabilities::HasAxis(uint16_t code) const {
    return FindAxis(code).has_value();
}

std::optional<Axis> Capabilities::FindAxis(uint16_t code) const {
    if (code >= axes.size()) {
        return std::nullopt;
    }
    const std::optional<Axis>& held = axes[code];
    if (!held || held->max <= held->min) {
        return std::nullopt;
    }
    return held;
}

Idc Idc::Parse(std::string_view text) {
    Idc idc;
    while (!text.empty()) {
        const size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        line = line.substr(0, line.find('#'));
        const size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const std::string_view key = Utils::Trim(line.substr(0, equals));
        const std::string_view value = Utils::Trim(line.substr(equals + 1));
        if (key == "touch.deviceType") {
            idc.deviceType = std::string(value);
        } else if (key == "touch.orientationAware") {
            idc.orientationAware = value != "0";
        } else if (key == "touch.displayId") {
            idc.display = std::string(value);
        } else if (key == "touch.orientation") {
            idc.orientation = value == "ORIENTATION_90" ? Rotation::Deg90 : value == "ORIENTATION_180" ? Rotation::Deg180 : value == "ORIENTATION_270" ? Rotation::Deg270 : Rotation::Deg0;
        }
    }
    return idc;
}

std::vector<std::string> Idc::FileNames(const Capabilities& device) {
    std::vector<std::string> names;
    if (device.vendor != 0 && device.product != 0) {
        if (device.version != 0) {
            names.push_back(std::format("Vendor_{:04x}_Product_{:04x}_Version_{:04x}.idc", device.vendor, device.product, device.version));
        }
        names.push_back(std::format("Vendor_{:04x}_Product_{:04x}.idc", device.vendor, device.product));
    }
    std::string canonical;
    for (const char c : device.name) {
        // A character outside ASCII is replaced once, not once for each of its bytes.
        if ((static_cast<unsigned char>(c) & 0xc0) == 0x80) {
            continue;
        }
        const bool kept = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
        canonical.push_back(kept ? c : '_');
    }
    names.push_back(canonical + ".idc");
    return names;
}

std::optional<std::pair<Axis, Axis>> Roles::Both(std::optional<Axis> x, std::optional<Axis> y) {
    if (!x || !y) {
        return std::nullopt;
    }
    return std::pair{*x, *y};
}

std::optional<Confidence> Roles::ScreenConfidence(const Capabilities& device, const Idc& idc, std::optional<std::string_view> display, Axis x, Axis y) {
    if (idc.display && display && *idc.display != *display) {
        return std::nullopt;
    }
    if (idc.deviceType == "touchScreen") {
        return Confidence::Certain();
    }
    if (idc.deviceType && idc.deviceType != "default") {
        return std::nullopt;
    }
    if (device.HasProperty(PropDirect)) {
        return Confidence::Certain();
    }
    if (device.HasProperty(PropPointer)) {
        return std::nullopt;
    }
    std::string name = device.name;
    for (char& c : name) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    // Names that rule a surface out when nothing better than a guess is available.
    static constexpr std::array<std::string_view, 17> NotAScreen = {
        "fingerprint", "finger_print", "fpc", "goodix_fp", "fp_", "gesture", "hall", "sensor", "accel", "gyro", "proximity", "keyboard", "mouse", "gpio-keys", "headset", "ir_", "hbtp",
    };
    if (std::ranges::any_of(NotAScreen, [&name](std::string_view word) { return name.contains(word); })) {
        return std::nullopt;
    }
    const int32_t span = std::min(x.Span(), y.Span());
    // A surface with fewer than 200 steps along a side is no screen.
    if (span < 200) {
        return std::nullopt;
    }
    return Confidence::Guess(span);
}

Roles Roles::Classify(const Capabilities& device, const Idc& idc, std::optional<std::string_view> display) {
    Roles roles;
    roles.keyboard = device.HasKey(KeyEnter) || device.HasKey(KeyA);
    roles.wheel = device.HasRelative(RelWheel) || device.HasRelative(RelHwheel);

    const bool gamepad = device.HasKeyIn(BtnJoystick, BtnDigi);
    const bool button = device.HasKey(BtnTouch);
    const bool mouse = device.HasKey(BtnLeft) && !gamepad;
    const std::optional<Axis> slot = device.FindAxis(AbsMtSlot);
    const bool slotted = slot && slot->min == 0 && device.HasAxis(AbsMtTrackingId);
    std::optional<std::pair<Axis, Axis>> multi = Both(device.FindAxis(AbsMtPositionX), device.FindAxis(AbsMtPositionY));
    if (!button && gamepad) {
        multi.reset();
    }
    const std::optional<std::pair<Axis, Axis>> single = Both(device.FindAxis(AbsX), device.FindAxis(AbsY));

    struct Surface {
        Protocol protocol;
        Axis x;
        Axis y;
        uint16_t pressure;
    };
    std::optional<Surface> surface;
    if (multi && slotted) {
        surface = Surface{Protocol::Slots(), multi->first, multi->second, AbsMtPressure};
    } else if (multi) {
        surface = Surface{Protocol::Frames(device.HasAxis(AbsMtTrackingId)), multi->first, multi->second, AbsMtPressure};
    } else if (single && button && !mouse) {
        // An absolute device with mouse buttons is a mouse, even where it also lists `BTN_TOUCH`.
        surface = Surface{Protocol::Single(), single->first, single->second, AbsPressure};
    }

    if (surface) {
        if (const std::optional<Confidence> confidence = ScreenConfidence(device, idc, display, surface->x, surface->y)) {
            roles.touch = Panel{
                .protocol = surface->protocol,
                .x = surface->x,
                .y = surface->y,
                .orientation = idc.orientation,
                .followsDisplay = idc.orientationAware.value_or(true),
                .confidence = *confidence,
                .button = button,
                .pressure = device.HasAxis(surface->pressure),
            };
        }
    } else if (single && mouse) {
        roles.pointer = Pointer{single->first, single->second};
    }
    return roles;
}

std::string Roles::Describe(bool screen, bool grabbed) const {
    std::vector<std::string> parts;
    if (touch) {
        const char* protocol = touch->protocol.kind == Protocol::Kind::Slots ? "Slots" : touch->protocol.kind == Protocol::Kind::Frames ? "Frames" : "Single";
        parts.push_back(std::format("touch {} {}x{}{}", protocol, touch->x.max, touch->y.max, screen ? " (screen)" : ""));
    }
    if (pointer) {
        parts.emplace_back("pointer");
    }
    if (keyboard) {
        parts.emplace_back("keyboard");
    }
    if (wheel) {
        parts.emplace_back("wheel");
    }
    if (grabbed) {
        parts.emplace_back("grabbed");
    }
    std::string line;
    for (const std::string& part : parts) {
        line += (line.empty() ? "" : ", ") + part;
    }
    return line;
}

std::pair<float, float> Panel::Rotate(float u, float v, Rotation rotation) {
    switch (rotation) {
        case Rotation::Deg0:
            return {u, v};
        case Rotation::Deg90:
            return {v, 1.0f - u};
        case Rotation::Deg180:
            return {1.0f - u, 1.0f - v};
        case Rotation::Deg270:
            return {1.0f - v, u};
    }
    return {u, v};
}

std::array<float, 2> Panel::ToScreen(float u, float v, std::array<float, 2> size, Rotation rotation) const {
    std::tie(u, v) = Rotate(u, v, orientation);
    if (followsDisplay) {
        std::tie(u, v) = Rotate(u, v, rotation);
    }
    return {u * size[0], v * size[1]};
}

std::pair<float, float> Panel::FromScreen(std::array<float, 2> position, std::array<float, 2> size, Rotation rotation) {
    const Rotation back = rotation == Rotation::Deg90 ? Rotation::Deg270 : rotation == Rotation::Deg270 ? Rotation::Deg90 : rotation;
    return Rotate(position[0] / size[0], position[1] / size[1], back);
}
