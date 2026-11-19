#pragma once

#include "Window/ANativeWindowCreator.h"

#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// The kernel's event codes under the kernel's own names. `<linux/input.h>` has the same names
// as macros, so a file that needs both has to include this header first.

inline constexpr uint16_t EvSyn = 0x00;
inline constexpr uint16_t EvKey = 0x01;
inline constexpr uint16_t EvRel = 0x02;
inline constexpr uint16_t EvAbs = 0x03;

inline constexpr uint16_t SynReport = 0;
inline constexpr uint16_t SynMtReport = 2;
inline constexpr uint16_t SynDropped = 3;

inline constexpr uint16_t RelHwheel = 0x06;
inline constexpr uint16_t RelWheel = 0x08;
inline constexpr uint16_t RelWheelHiRes = 0x0b;
inline constexpr uint16_t RelHwheelHiRes = 0x0c;

inline constexpr uint16_t AbsX = 0x00;
inline constexpr uint16_t AbsY = 0x01;
inline constexpr uint16_t AbsPressure = 0x18;
inline constexpr uint16_t AbsMtSlot = 0x2f;
inline constexpr uint16_t AbsMtFirst = 0x30;
inline constexpr uint16_t AbsMtLast = 0x3d;
inline constexpr uint16_t AbsMtPositionX = 0x35;
inline constexpr uint16_t AbsMtPositionY = 0x36;
inline constexpr uint16_t AbsMtToolType = 0x37;
inline constexpr uint16_t AbsMtTrackingId = 0x39;
inline constexpr uint16_t AbsMtPressure = 0x3a;
inline constexpr int32_t MtToolPalm = 2;
inline constexpr size_t AbsCount = 0x40;

inline constexpr uint16_t KeyEnter = 28;
inline constexpr uint16_t KeyA = 30;
inline constexpr uint16_t BtnMisc = 0x100;
inline constexpr uint16_t BtnLeft = 0x110;
inline constexpr uint16_t BtnRight = 0x111;
inline constexpr uint16_t BtnMiddle = 0x112;
inline constexpr uint16_t BtnJoystick = 0x120;
inline constexpr uint16_t BtnDigi = 0x140;
inline constexpr uint16_t BtnTouch = 0x14a;
inline constexpr size_t KeyCount = 0x300;

inline constexpr uint32_t PropPointer = 0;
inline constexpr uint32_t PropDirect = 1;

struct RawEvent {
    uint16_t kind = 0;
    uint16_t code = 0;
    int32_t value = 0;

    bool operator==(const RawEvent&) const = default;

    // Decodes kernel `input_event` records, whose timestamp takes `timeSize` bytes, onto the
    // end of `out`.
    static void Decode(std::span<const uint8_t> bytes, size_t timeSize, std::vector<RawEvent>& out);

    // Appends the record the kernel expects on a write; it fills in the timestamp itself.
    void Encode(size_t timeSize, std::vector<uint8_t>& bytes) const;
};

// What a device's absolute axes hold: the slot it addresses and every axis's value, one per
// slot for the multi-touch axes of a slotted device.
struct AxisState {
    int32_t slot = 0;
    std::vector<std::pair<uint16_t, std::vector<int32_t>>> values;

    bool operator==(const AxisState&) const = default;

    // The events that take a device from this state back to an `earlier` one. The kernel only
    // reports changes, so a reader that missed everything in between would otherwise go on
    // with stale values.
    void Rewind(const AxisState& earlier, std::vector<RawEvent>& out) const;

    // What one axis holds, slot by slot; nothing for an axis the device does not have.
    std::span<const int32_t> Column(uint16_t code) const;
};

struct Axis {
    int32_t min = 0;
    int32_t max = 0;

    bool operator==(const Axis&) const = default;

    int32_t Span() const;

    // Position inside the axis as a fraction in `0.0..1.0`, the way Android's input reader scales it.
    float Normalize(int32_t value) const;
};

struct Capabilities {
    std::string name;
    uint16_t bus = 0;
    uint16_t vendor = 0;
    uint16_t product = 0;
    uint16_t version = 0;
    uint32_t properties = 0;
    uint32_t relative = 0;
    std::vector<uint8_t> keys;
    std::vector<std::optional<Axis>> axes;

    bool HasProperty(uint32_t property) const;
    bool HasRelative(uint16_t code) const;
    bool HasKey(uint16_t code) const;
    // Whether the device has any key from `begin` up to, not including, `end`.
    bool HasKeyIn(uint16_t begin, uint16_t end) const;
    bool HasAxis(uint16_t code) const;
    std::optional<Axis> FindAxis(uint16_t code) const;
};

struct Protocol {
    enum class Kind : uint8_t { Slots, Frames, Single };

    Kind kind = Kind::Slots;
    // Only `Frames` has it: whether its contacts come with tracking ids.
    bool tracked = false;

    static constexpr Protocol Slots() {
        return {Kind::Slots, false};
    }
    static constexpr Protocol Frames(bool withIds) {
        return {Kind::Frames, withIds};
    }
    static constexpr Protocol Single() {
        return {Kind::Single, false};
    }

    bool operator==(const Protocol&) const = default;
};

// How sure we are that a touch surface is the screen itself.
struct Confidence {
    enum class Kind : uint8_t { Guess, Certain };

    Kind kind = Kind::Guess;
    // Only a `Guess` has it: the wider the surface, the better the guess.
    int32_t span = 0;

    static constexpr Confidence Guess(int32_t shorterSpan) {
        return {Kind::Guess, shorterSpan};
    }
    static constexpr Confidence Certain() {
        return {Kind::Certain, 0};
    }

    auto operator<=>(const Confidence&) const = default;
};

struct Panel {
    Protocol protocol;
    Axis x;
    Axis y;
    Rotation orientation = Rotation::Deg0;
    bool followsDisplay = false;
    Confidence confidence;
    // The device has `BTN_TOUCH`: while it is up, whatever it reports hovers.
    bool button = false;
    // The device reports pressure: a contact without any hovers.
    bool pressure = false;

    bool operator==(const Panel&) const = default;

    // Turns a position given as fractions of an unrotated surface into fractions of the same
    // surface after `rotation`, matching Android's input reader.
    static std::pair<float, float> Rotate(float u, float v, Rotation rotation);
    // Maps a position on this panel to pixels of a display of `size` in its current orientation.
    std::array<float, 2> ToScreen(float u, float v, std::array<float, 2> size, Rotation rotation) const;
    // The reverse of `ToScreen` for a panel that lies the way the display naturally does.
    static std::pair<float, float> FromScreen(std::array<float, 2> position, std::array<float, 2> size, Rotation rotation);
};

struct Pointer {
    Axis x;
    Axis y;

    bool operator==(const Pointer&) const = default;

    // Absolute pointers (emulator mice) already report in the display's current orientation.
    static constexpr std::array<float, 2> ToScreen(float u, float v, std::array<float, 2> size) {
        return {u * size[0], v * size[1]};
    }
};

struct Idc;

struct Roles {
    std::optional<Panel> touch;
    std::optional<Pointer> pointer;
    bool keyboard = false;
    bool wheel = false;

    bool operator==(const Roles&) const = default;

    bool IsEmpty() const {
        return !touch && !pointer && !keyboard && !wheel;
    }

    // Works out what a device is from what it can report, the way Android's own event hub does:
    // names differ between emulators and even between their versions, capabilities do not.
    // The display is given by the unique id of the one the overlay is on, when it is known.
    static Roles Classify(const Capabilities& device, const Idc& idc, std::optional<std::string_view> display);

    // The roles in words, for the list of devices; `screen` and `grabbed` are what the reader
    // made of the device.
    std::string Describe(bool screen, bool grabbed) const;

private:
    static std::optional<std::pair<Axis, Axis>> Both(std::optional<Axis> x, std::optional<Axis> y);
    static std::optional<Confidence> ScreenConfidence(const Capabilities& device, const Idc& idc, std::optional<std::string_view> display, Axis x, Axis y);
};

// The subset of an Android input device configuration file that affects touch mapping.
struct Idc {
    std::optional<std::string> deviceType;
    Rotation orientation = Rotation::Deg0;
    std::optional<bool> orientationAware;
    std::optional<std::string> display;

    bool operator==(const Idc&) const = default;

    static Idc Parse(std::string_view text);

    // File names Android looks up for a device, most specific first.
    static std::vector<std::string> FileNames(const Capabilities& device);
};
