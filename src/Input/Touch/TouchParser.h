#pragma once

#include "Input/Touch/TouchDevice.h"
#include "Window/ANativeWindowCreator.h"

#include <array>
#include <chrono>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

enum class Phase : uint8_t { Start, Move, End, Cancel };

struct Contact {
    uint32_t id = 0;
    Phase phase = Phase::Start;
    float u = 0.0f;
    float v = 0.0f;

    bool operator==(const Contact&) const = default;
};

// Turns the events of one touch surface into contacts. Times are counted from one starting
// point of the caller's choosing, the same for every call. This class is what the protocols
// share; `Create` gives the parser of a panel's own.
class TouchParser {
public:
    static std::unique_ptr<TouchParser> Create(const Panel& setup);
    virtual ~TouchParser() = default;

    // When `Tick` has to run even if the device stays silent.
    virtual std::optional<std::chrono::nanoseconds> Deadline() const;

    virtual void Feed(RawEvent event, std::chrono::nanoseconds now, std::vector<Contact>& out) = 0;

    virtual void Tick(std::chrono::nanoseconds now, std::vector<Contact>& out);

    // Whether no finger is down.
    virtual bool IsIdle() const = 0;

    // Drops every contact, for a device that went away or lost events.
    virtual void Cancel(std::vector<Contact>& out) = 0;

    // Loads the slot state read straight from the device, and whether its touch button is
    // down. Only a panel with slots takes anything from it.
    virtual void RestoreSlots(const AxisState& state, bool pressed, std::vector<Contact>& out);

    // Whether a device in this state, with its touch button `pressed` or not, has a finger on
    // it. A slot that merely holds a tracking id does not: some drivers leave zero in slots
    // that were never used.
    bool IsTouching(const AxisState& state, bool pressed) const;

protected:
    struct Point {
        int32_t x = 0;
        int32_t y = 0;
        std::optional<int32_t> tracking;
    };

    // A finger that is down, as the protocols without slots follow it.
    struct Live {
        uint32_t id = 0;
        Point point;
    };

    // No protocol here follows more fingers than this.
    static constexpr size_t MaxContacts = 32;

    explicit TouchParser(const Panel& setup);

    // Android's rule: a finger hovers while the touch button is up or it presses on nothing.
    bool Hovers(bool pressed, int32_t pressure, bool palm) const;
    Contact MakeContact(uint32_t id, Phase phase, const Point& point) const;
    uint32_t FreshId();
    static int32_t ValueAt(std::span<const int32_t> values, size_t slot);

    Panel setup_;
    // Whether the device's touch button is down.
    bool pressed_ = false;

private:
    uint32_t nextId_ = 0;
};

// Multi-touch protocol B: every finger reports into a slot of its own. Phones and tablets.
class SlotsParser final : public TouchParser {
public:
    explicit SlotsParser(const Panel& setup);

    void Feed(RawEvent event, std::chrono::nanoseconds now, std::vector<Contact>& out) override;
    bool IsIdle() const override;
    void Cancel(std::vector<Contact>& out) override;
    void RestoreSlots(const AxisState& state, bool pressed, std::vector<Contact>& out) override;

private:
    struct Held {
        uint32_t id = 0;
        int32_t tracking = 0;
    };

    struct Slot {
        int32_t tracking = -1;
        int32_t x = 0;
        int32_t y = 0;
        int32_t pressure = 0;
        bool palm = false;
        std::optional<Held> live;
        bool moved = false;
    };

    Slot& CurrentSlot();
    void Flush(std::vector<Contact>& out);

    std::vector<Slot> slots_;
    size_t current_ = 0;
};

// Multi-touch protocol A: the device reports whole frames of points, with tracking ids or
// without. Emulators mostly.
//
// Panels that report frames with neither tracking ids nor `BTN_TOUCH` drop a held contact for a
// moment and then resume it, so their release is only believed once the panel has stayed empty:
// briefly for a fresh contact, to keep taps quick, and longer once a contact has been held for
// more than a tap.
class FramesParser final : public TouchParser {
public:
    explicit FramesParser(const Panel& setup);

    std::optional<std::chrono::nanoseconds> Deadline() const override;
    void Feed(RawEvent event, std::chrono::nanoseconds now, std::vector<Contact>& out) override;
    void Tick(std::chrono::nanoseconds now, std::vector<Contact>& out) override;
    bool IsIdle() const override;
    void Cancel(std::vector<Contact>& out) override;

private:
    struct Partial {
        std::optional<int32_t> x;
        std::optional<int32_t> y;
        std::optional<int32_t> tracking;
        std::optional<int32_t> pressure;
    };

    void PushPartial();
    void Commit(std::chrono::nanoseconds now, std::vector<Contact>& out);
    void Finish(Phase phase, std::vector<Contact>& out);

    Partial partial_;
    Point last_;
    std::vector<Point> frame_;
    std::vector<Live> live_;
    std::optional<std::chrono::nanoseconds> heldSince_;
    std::optional<std::chrono::nanoseconds> releaseAt_;
    bool fading_ = false;
};

// One finger: plain `ABS_X` and `ABS_Y` with a touch button.
class SingleParser final : public TouchParser {
public:
    explicit SingleParser(const Panel& setup);

    void Feed(RawEvent event, std::chrono::nanoseconds now, std::vector<Contact>& out) override;
    bool IsIdle() const override;
    void Cancel(std::vector<Contact>& out) override;

private:
    void Finish(Phase phase, std::vector<Contact>& out);

    // What has come since the last report.
    std::optional<int32_t> x_;
    std::optional<int32_t> y_;
    Point last_;
    std::optional<Live> live_;
    int32_t pressure_ = 0;
};

// The name the virtual touchscreen goes by in `/dev/input`.
inline constexpr std::string_view VirtualTouchscreen = "android-imgui touch";
// Android follows at most this many pointers at once.
inline constexpr size_t VirtualSlots = 16;
// Units along each axis of the virtual touchscreen.
inline constexpr int32_t VirtualRange = 32768;

// Whose finger it is: a contact of a touch panel that is passed on, or one made from code.
struct Owner {
    enum class Kind : uint8_t { Panel, Code };

    Kind kind = Kind::Panel;
    // Only a `Panel` finger has these.
    uint32_t device = 0;
    uint32_t contact = 0;
    // Only a `Code` finger has it.
    uint32_t finger = 0;

    static constexpr Owner Panel(uint32_t index, uint32_t id) {
        return {Kind::Panel, index, id, 0};
    }
    static constexpr Owner Code(uint32_t number) {
        return {Kind::Code, 0, 0, number};
    }

    bool operator==(const Owner&) const = default;
};

// The fingers on the virtual touchscreen, and the events that put them there.
class Fingers {
public:
    bool IsEmpty() const;

    bool HoldsPanel(uint32_t wanted) const;

    // Writes the events for one change of a finger; `at` is a fraction of the display in its
    // natural orientation. Returns `false` when there is nothing to change.
    bool Apply(Owner owner, Phase phase, std::pair<float, float> at, std::vector<RawEvent>& out);

    // Lifts every finger `which` picks.
    template <std::predicate<Owner> Which>
    void Lift(Which which, std::vector<RawEvent>& out) {
        for (const std::optional<Owner>& owner : std::array(owners_)) {
            if (owner && which(*owner)) {
                Apply(*owner, Phase::End, {0.0f, 0.0f}, out);
            }
        }
    }

private:
    std::array<std::optional<Owner>, VirtualSlots> owners_;
    int32_t tracking_ = 0;
};

// A touch as Android itself reports it: what happened, to which of the fingers, and where
// every finger is, in display pixels. It is the source of touches on systems that have no
// touch devices to read, such as Android in a container.
struct Motion {
    uint32_t action = 0;
    size_t index = 0;
    std::vector<std::pair<uint32_t, std::array<float, 2>>> pointers;

    bool operator==(const Motion&) const = default;

    // Reads the helper's line: `touch <action> <index>` and then `<id> <x> <y>` for each finger.
    static std::optional<Motion> Parse(std::string_view line);

    // What the motion means for each finger.
    std::vector<std::pair<Contact, std::array<float, 2>>> Contacts() const;

private:
    // The next field of a line, taken off its front.
    static std::optional<std::string_view> NextField(std::string_view& line);

    template <std::unsigned_integral T>
    static std::optional<T> ParseUnsigned(std::string_view text);

    // A decimal number the way Java writes one: digits with an optional point and exponent,
    // `Infinity` or `NaN`, and nothing around them.
    static std::optional<float> ParseJavaFloat(std::string_view text);
};

enum class MouseButton : uint8_t { Left, Right, Middle };

// One input change in device space: positions are fractions of the device's own axes.
struct Sample {
    struct Hover {
        float u;
        float v;

        bool operator==(const Hover&) const = default;
    };

    struct Button {
        MouseButton button;
        bool pressed;

        bool operator==(const Button&) const = default;
    };

    struct Wheel {
        float x;
        float y;

        bool operator==(const Wheel&) const = default;
    };

    struct Key {
        uint16_t code;
        bool pressed;
        bool repeat;

        bool operator==(const Key&) const = default;
    };

    std::variant<Contact, Hover, Button, Wheel, Key> value;

    bool operator==(const Sample&) const = default;
};

class DeviceParser {
public:
    DeviceParser(const Capabilities& device, const Roles& roles);

    // The parser of the device's touch surface; null when it has none.
    TouchParser* TouchMut();

    std::optional<std::chrono::nanoseconds> Deadline() const;

    bool IsIdle() const;

    // Whether a device in this state has a finger on its touch surface.
    bool IsTouching(const AxisState& state, bool pressed) const;

    void Feed(RawEvent event, std::chrono::nanoseconds now, std::vector<Sample>& out);

    void Tick(std::chrono::nanoseconds now, std::vector<Sample>& out);

    void Cancel(std::vector<Sample>& out);

private:
    void PassContacts(std::vector<Sample>& out);
    void Flush(std::vector<Sample>& out);

    std::unique_ptr<TouchParser> touch_;
    std::optional<Pointer> pointer_;
    bool keyboard_ = false;
    bool wheel_ = false;
    bool preciseWheel_ = false;
    std::array<int32_t, 2> position_{};
    bool moved_ = false;
    std::vector<Sample::Button> buttons_;
    std::array<float, 2> scroll_{};
    std::vector<Contact> contacts_;
};
