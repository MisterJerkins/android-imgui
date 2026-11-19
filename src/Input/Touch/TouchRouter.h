#pragma once

#include "Input/Touch/TouchDevice.h"
#include "Input/Touch/TouchParser.h"
#include "Utils/Utils.h"
#include "Window/ANativeWindowCreator.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

enum class TouchTarget : uint8_t { Interface, Keyboard, Outside };

// What the interface looked like after the last frame, in display pixels.
struct InterfaceView {
    std::array<float, 2> size{};
    Rotation rotation = Rotation::Deg0;
    std::vector<Area> regions;
    std::optional<Area> unused;
    std::optional<Area> keyboard;
    bool dismissable = false;
    bool wantsKeys = false;

    bool operator==(const InterfaceView&) const = default;

    TouchTarget FindTarget(std::array<float, 2> position) const;
};

struct InputEvent {
    struct Touch {
        uint32_t device;
        uint32_t id;
        Phase phase;
        std::array<float, 2> position;

        bool operator==(const Touch&) const = default;
    };

    struct PointerMoved {
        std::array<float, 2> position;

        bool operator==(const PointerMoved&) const = default;
    };

    struct PointerButton {
        std::array<float, 2> position;
        MouseButton button;
        bool pressed;

        bool operator==(const PointerButton&) const = default;
    };

    struct PointerGone {
        bool operator==(const PointerGone&) const = default;
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

    struct KeyboardTouch {
        uint64_t id;
        Phase phase;
        std::array<float, 2> position;

        bool operator==(const KeyboardTouch&) const = default;
    };

    std::variant<Touch, PointerMoved, PointerButton, PointerGone, Wheel, Key, KeyboardTouch> value;

    bool operator==(const InputEvent&) const = default;

    bool IsButton(bool pressed) const;
    bool IsLift() const;
    bool IsPointerMotion() const;
    bool IsMotion() const;
    // Whether two movements are of the same finger, or both of the pointer.
    bool IsSameMotion(const InputEvent& event) const;
};

// Decides which touches belong to the interface. A contact is judged once, where it lands:
// fingers resting on whatever is underneath never reach the interface, however long they stay down.
class TouchRouter {
public:
    // Returns where the contact went: the same answer for its whole life.
    TouchTarget RouteContact(uint32_t device, Contact contact, std::array<float, 2> position, const InterfaceView& view, std::vector<InputEvent>& out);

    void RouteHover(std::array<float, 2> position, const InterfaceView& view, std::vector<InputEvent>& out);

    void RouteButton(MouseButton which, bool pressed, const InterfaceView& view, std::vector<InputEvent>& out);

    // Scrolls whatever the pointer is over. Emulators that turn the mouse into touches send
    // the wheel without a pointer; it then applies where the last touch was.
    void RouteWheel(float x, float y, const InterfaceView& view, std::vector<InputEvent>& out);

    void RouteKey(uint16_t code, bool pressed, bool repeat, const InterfaceView& view, std::vector<InputEvent>& out);

private:
    bool IsDragging() const;

    // A press outside the interface still has to close popups and take focus off text fields.
    void Dismiss(std::array<float, 2> position, const InterfaceView& view, std::vector<InputEvent>& out);

    // Where each contact went, under its device and id in one number.
    std::unordered_map<uint64_t, TouchTarget> routes_;
    std::optional<uint64_t> owner_;
    std::array<float, 2> mouse_{};
    bool hovering_ = false;
    std::array<bool, 3> held_{};
    bool typing_ = false;
};

// Events waiting for the next frame, each with the moment it happened. Runs of movement
// collapse to their latest position, so a stalled frame or a 1000 Hz panel cannot flood it;
// presses and releases are never lost.
class EventQueue {
public:
    bool IsEmpty() const;

    // Takes the events for one frame, cut so that the interface sees a drag whole: a press
    // ends its frame, and a release waits for the next one when movement came before it.
    // Returns when the last of the frame's events happened.
    std::optional<std::chrono::steady_clock::time_point> TakeFrame(std::vector<InputEvent>& into);

    void Push(InputEvent event, std::chrono::steady_clock::time_point at);

private:
    std::vector<InputEvent> events_;
    std::vector<std::chrono::steady_clock::time_point> times_;
};
