#pragma once

#include "Input/Touch/Touch.h"
#include "Input/Touch/TouchDevice.h"
#include "Input/Touch/TouchParser.h"
#include "Input/Touch/TouchRouter.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class Fd;

// What `TouchReader::Drain` says about the input it handed over.
struct Taken {
    // When the last of it happened.
    std::optional<std::chrono::steady_clock::time_point> at;
    // Whether some of the input since the last call was kept from the system, which then
    // does not know the user is there.
    bool kept = false;
};

// Reads touch, mouse and keyboard devices, one thread per device. Devices are opened for
// reading and left alone, so whatever is under the interface keeps its input, until the
// injector is told to take control.
class TouchReader {
public:
    // `display` is the unique id of the display the overlay is on, for systems that tie each
    // touchscreen to one display.
    static TouchReader Start(std::optional<std::string> display);

    // Ends the reading: control goes back to Android, and each thread leaves the next time
    // it wakes up.
    void Stop() const;

    // Hands over the latest view and takes the previous one back, so its buffers are reused.
    void SwapView(InterfaceView& view) const;

    // What takes control of the touches and plays fingers of its own.
    std::shared_ptr<TouchInjector> GetInjector() const;

    // Whether there is a touchscreen among the input devices. The answer waits until all of
    // them have been looked at once, which a busy system can take its time over, but for no
    // longer than `patience`.
    bool HasScreen(std::chrono::milliseconds patience) const;

    // Switches between touches reported by the system and those of touch devices, and
    // returns where to send the system's.
    std::function<void(const Motion&)> SystemTouches(bool enabled) const;

    // Blocks until input is waiting or `timeout` passes. Returns whether input is waiting.
    bool Wait(std::chrono::nanoseconds timeout) const;

    // Takes the input for one frame; whatever is left makes `wait` return at once.
    Taken Drain(std::vector<InputEvent>& into) const;

    // Input devices in use, one line each.
    std::vector<std::string> Describe() const;

    // What the kernel says about an event device. Every request here is a getter: nothing is
    // written to the device.

    // How many bytes the timestamp of a kernel `input_event` takes on this system.
    static const size_t TimeSize;
    // Reads what a device can report.
    static std::optional<Capabilities> ReadCapabilities(int device);
    // Whether a key or button is held down right now.
    static bool IsKeyDown(int device, uint16_t code);
    // What the given axes hold right now, with one value per slot for multi-touch axes.
    static AxisState ReadAxisState(int device, std::span<const uint16_t> axes, size_t slots);

private:
    class Hub;
    struct InputDevice;
    struct ControlState;
    struct RoutingState;
    struct ReaderState;

    explicit TouchReader(std::shared_ptr<Hub> hub) : hub_(std::move(hub)) {
    }

    static constexpr uint32_t ReadRequest(uint32_t number, size_t size) {
        return 2u << 30 | static_cast<uint32_t>(size) << 16 | static_cast<uint32_t>('E') << 8 | number;
    }

    static bool Query(int device, uint32_t number, void* target, size_t size);

    template <class T>
    static bool Query(int device, uint32_t number, T& target);

    static constexpr uint32_t Request(bool writes, char group, uint32_t number) {
        const uint32_t base = static_cast<uint32_t>(group) << 8 | number;
        return writes ? 1u << 30 | 4u << 16 | base : base;
    }

    static Result<> Command(int device, uint32_t what, uint32_t value);

    // Creates a multi-touch screen whose axes run from zero to `range - 1`. It lives as long as
    // the returned descriptor stays open.
    static Result<Fd> CreateVirtualTouchscreen(std::string_view name, int32_t range, size_t slots);

    // Takes a device away from every other reader, or gives it back.
    static Result<> GrabDevice(int device, bool grab);

    static Idc LoadIdc(const Capabilities& device);

    static std::optional<InputDevice> OpenDevice(const std::string& path, std::optional<std::string_view> display);

    std::shared_ptr<Hub> hub_;
};
