#pragma once

#include "Input/Touch/TouchRouter.h"
#include "Utils/Utils.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

class Graphics;

// What `Touch` drives: the input devices, or a stand-in under test.
class TouchInjector {
public:
    virtual ~TouchInjector() = default;

    virtual Result<> SetControl(bool enabled) = 0;
    virtual bool IsControl() const = 0;
    virtual Result<> MoveFinger(uint32_t finger, Phase phase, std::array<float, 2> position) = 0;
};

// The frames' side of the input: what `Touch::Init` gives the graphics.
class TouchSession;

// The input of the interface: touches, a mouse and, with them, the keyboards. There is one,
// `touch`. The fingers it plays may be driven from any thread; the rest belongs to the
// thread that draws the frames.
//
// Touches are only read until `SetControl` turns control on. From then on a virtual
// touchscreen stands in for the real one: fingers on the interface stay with the interface,
// everything else is passed on, and `down`, `move` and `up` add fingers of their own.
// Positions are display pixels in the current orientation.
//
// What can fail returns `false`; the reason is in the log and in `GetError`.
class Touch {
public:
    Touch();
    // A touch that drives `injector` and nothing else, for tests.
    explicit Touch(std::shared_ptr<TouchInjector> injector);
    Touch(const Touch&) = delete;
    Touch& operator=(const Touch&) = delete;
    ~Touch();

    // Starts reading the input devices and gives the frames of `graphics` their input. Touches
    // are read only, and whatever is under the interface keeps getting them, unless `readOnly`
    // is false: that is `SetControl(true)` from the start.
    bool Init(Graphics& graphics, bool readOnly = true);
    // Stops reading and gives back what control took. Done by itself when the program ends.
    void Close();

    bool SetControl(bool enabled);
    bool IsControl() const;

    bool Down(uint32_t finger, std::array<float, 2> position);
    bool Move(uint32_t finger, std::array<float, 2> position);
    bool Up(uint32_t finger);

    // Taps once. Blocks for the length of the tap.
    bool Tap(std::array<float, 2> position);
    // Drags one finger in a straight line. Blocks until it is lifted.
    bool Swipe(std::array<float, 2> from, std::array<float, 2> to, std::chrono::milliseconds duration);

    // The input the frame being drawn was given, in display pixels: what the interface
    // received after routing, for an application that wants to look at it itself.
    std::span<const InputEvent> GetEvents() const;
    // Input devices in use, one line each.
    std::vector<std::string> GetDevices() const;
    // What went wrong when something last returned `false`.
    std::string GetError() const;

private:
    // Keeps the reason of a failure, tells the log, and is the `false` to return.
    bool Fail(const Error& error);
    bool Play(uint32_t finger, Phase phase, std::array<float, 2> position);
    std::shared_ptr<TouchInjector> GetInjector() const;

    std::shared_ptr<TouchSession> session_;
    // What other threads may ask for while this one starts or closes.
    mutable std::mutex mutex_;
    std::shared_ptr<TouchInjector> injector_;
    std::string error_;
};

inline Touch touch;
