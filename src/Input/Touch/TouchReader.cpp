#include "Input/Touch/TouchReader.h"

#include "Graphics/Graphics.h"
#include "Helper/Helper.h"
#include "Input/Keyboard/KeyboardManager.h"
#include "Input/Touch/ImGuiBridge.h"
#include "Input/Touch/TouchDevice.h"
#include "Input/Touch/TouchParser.h"
#include "Utils/Logger.h"
#include "Utils/System.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <format>
#include <mutex>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

const size_t TouchReader::TimeSize = sizeof(timeval);

bool TouchReader::Query(int device, uint32_t number, void* target, size_t size) {
    return ioctl(device, ReadRequest(number, size), target) >= 0;
}

template <class T>
bool TouchReader::Query(int device, uint32_t number, T& target) {
    return Query(device, number, &target, sizeof target);
}

std::optional<Capabilities> TouchReader::ReadCapabilities(int device) {
    char name[128] = {};
    uint16_t identity[4] = {};
    uint8_t properties[4] = {};
    uint8_t relative[4] = {};
    uint8_t axes[AbsCount / 8] = {};
    Capabilities found;
    found.keys.assign(KeyCount / 8, 0);
    found.axes.assign(AbsCount, std::nullopt);

    if (!Query(device, 0x02, identity)) {
        return std::nullopt;
    }
    Query(device, 0x06, name, sizeof name - 1);
    Query(device, 0x09, properties);
    Query(device, 0x20 + uint32_t{EvRel}, relative);
    Query(device, 0x20 + uint32_t{EvAbs}, axes);
    Query(device, 0x20 + uint32_t{EvKey}, found.keys.data(), found.keys.size());

    found.name = name;
    found.bus = identity[0];
    found.vendor = identity[1];
    found.product = identity[2];
    found.version = identity[3];
    std::memcpy(&found.properties, properties, sizeof found.properties);
    std::memcpy(&found.relative, relative, sizeof found.relative);
    for (size_t code = 0; code < AbsCount; code++) {
        if (!(axes[code / 8] & (1 << (code % 8)))) {
            continue;
        }
        int32_t info[6] = {};
        if (Query(device, 0x40 + static_cast<uint32_t>(code), info)) {
            found.axes[code] = Axis{info[1], info[2]};
        }
    }
    return found;
}

bool TouchReader::IsKeyDown(int device, uint16_t code) {
    uint8_t keys[KeyCount / 8] = {};
    return Query(device, 0x18, keys) && (keys[code / 8] & (1 << (code % 8))) != 0;
}

AxisState TouchReader::ReadAxisState(int device, std::span<const uint16_t> axes, size_t slots) {
    AxisState state;
    for (const uint16_t code : axes) {
        if (code >= AbsMtFirst && code <= AbsMtLast) {
            std::vector<int32_t> values(slots + 1, 0);
            values[0] = code;
            if (slots > 0 && Query(device, 0x0a, values.data(), values.size() * sizeof(int32_t))) {
                values.erase(values.begin());
                state.values.emplace_back(code, std::move(values));
            }
            continue;
        }
        int32_t info[6] = {};
        if (!Query(device, 0x40 + uint32_t{code}, info)) {
            continue;
        }
        if (code == AbsMtSlot) {
            state.slot = info[0];
        } else {
            state.values.emplace_back(code, std::vector<int32_t>{info[0]});
        }
    }
    return state;
}

constexpr size_t NameSize = 80;
constexpr uint16_t BusVirtual = 0x06;

Result<> TouchReader::Command(int device, uint32_t what, uint32_t value) {
    if (ioctl(device, what, static_cast<unsigned long>(value)) < 0) {
        return Err("{}", std::strerror(errno));
    }
    return {};
}

Result<Fd> TouchReader::CreateVirtualTouchscreen(std::string_view name, int32_t range, size_t slots) {
    constexpr uint32_t DeviceCreate = Request(false, 'U', 1);
    constexpr uint32_t SetEvent = Request(true, 'U', 100);
    constexpr uint32_t SetKey = Request(true, 'U', 101);
    constexpr uint32_t SetAbsolute = Request(true, 'U', 103);
    constexpr uint32_t SetProperty = Request(true, 'U', 110);
    Fd device;
    int failure = ENOENT;
    for (const char* path : {"/dev/uinput", "/dev/input/uinput", "/dev/misc/uinput"}) {
        device = Fd(open(path, O_WRONLY | O_CLOEXEC));
        if (device) {
            break;
        }
        failure = errno;
    }
    if (!device) {
        return Err("{}", std::strerror(failure));
    }
    const std::pair<uint16_t, int32_t> axes[] = {{AbsMtSlot, static_cast<int32_t>(slots) - 1}, {AbsMtTrackingId, 0xffff}, {AbsMtPositionX, range - 1}, {AbsMtPositionY, range - 1}};

    for (const uint16_t kind : {EvSyn, EvKey, EvAbs}) {
        TRY(Command(device.Get(), SetEvent, kind));
    }
    TRY(Command(device.Get(), SetKey, BtnTouch));
    TRY(Command(device.Get(), SetProperty, PropDirect));
    for (const auto& [axis, maximum] : axes) {
        TRY(Command(device.Get(), SetAbsolute, axis));
    }

    // struct uinput_user_dev: name, input_id, ff_effects_max, then absmax, absmin, absfuzz and absflat.
    std::vector<uint8_t> setup(NameSize + 12 + 4 * 4 * AbsCount, 0);
    std::memcpy(setup.data(), name.data(), std::min(name.size(), NameSize - 1));
    std::memcpy(setup.data() + NameSize, &BusVirtual, sizeof BusVirtual);
    for (const auto& [axis, maximum] : axes) {
        std::memcpy(setup.data() + NameSize + 12 + 4 * size_t{axis}, &maximum, sizeof maximum);
    }
    if (write(device.Get(), setup.data(), setup.size()) != static_cast<ssize_t>(setup.size())) {
        return Err("{}", std::strerror(errno));
    }
    TRY(Command(device.Get(), DeviceCreate, 0));
    return device;
}

Result<> TouchReader::GrabDevice(int device, bool grab) {
    constexpr uint32_t GrabRequest = Request(true, 'E', 0x90);
    return Command(device, GrabRequest, grab ? 1 : 0);
}

using Clock = std::chrono::steady_clock;
using std::chrono::nanoseconds;
using namespace std::chrono_literals;

// Where event devices live; the second place is where containers put the ones they create.
constexpr const char* DeviceDirectories[] = {"/dev/input", "/dev"};

struct TouchReader::InputDevice {
    std::string path;
    std::string name;
    Roles roles;
    DeviceParser parser;
    size_t slots = 0;
    bool screen = false;
    std::shared_ptr<Fd> file;
    std::vector<uint16_t> axes;
    uint32_t keysDown = 0;
    bool grabbed = false;
    bool refused = false;
    // What the axes held when the device was grabbed, which is how Android remembers them.
    std::optional<AxisState> left = std::nullopt;

    // Takes the device away from Android, or gives it back the way Android last saw it.
    Result<> Grab(bool wanted) {
        if (!wanted) {
            Rewind();
        }
        TRY(GrabDevice(file->Get(), wanted));
        AxisState state = TouchReader::ReadAxisState(file->Get(), axes, slots);
        // A finger that came down just before the grab is Android's, which saw it land.
        if (wanted && parser.IsTouching(state, TouchReader::IsKeyDown(file->Get(), BtnTouch))) {
            return GrabDevice(file->Get(), false);
        }
        grabbed = wanted;
        left = wanted ? std::optional(std::move(state)) : std::nullopt;
        return {};
    }

    void Rewind() {
        if (!left) {
            return;
        }
        const AxisState earlierState = std::move(*left);
        left.reset();
        std::vector<RawEvent> events;
        std::vector<uint8_t> bytes;
        TouchReader::ReadAxisState(file->Get(), axes, slots).Rewind(earlierState, events);
        for (const RawEvent& event : events) {
            event.Encode(TouchReader::TimeSize, bytes);
        }
        if (bytes.empty()) {
            return;
        }
        const Fd writer(open(path.c_str(), O_WRONLY | O_CLOEXEC));
        if (!writer || write(writer.Get(), bytes.data(), bytes.size()) != static_cast<ssize_t>(bytes.size())) {
            logger.Debug("{}: cannot rewind: {}", path, std::strerror(errno));
        }
    }
};

// The virtual touchscreen that stands in for the real one while touches are under control.
struct TouchReader::ControlState {
    Fd device;
    Fingers fingers;
    nanoseconds ready{};
    std::vector<RawEvent> events;
    std::vector<uint8_t> bytes;

    bool Send(Owner owner, Phase phase, std::pair<float, float> at) {
        const bool changed = fingers.Apply(owner, phase, at, events);
        Flush();
        return changed;
    }

    void Flush() {
        bytes.clear();
        for (const RawEvent& event : events) {
            event.Encode(TouchReader::TimeSize, bytes);
        }
        events.clear();
        if (!bytes.empty() && write(device.Get(), bytes.data(), bytes.size()) != static_cast<ssize_t>(bytes.size())) {
            logger.Warn("virtual touchscreen: {}", std::strerror(errno));
        }
    }
};

// Where input goes once a device has reported it.
struct TouchReader::RoutingState {
    InterfaceView view;
    TouchRouter router;
    EventQueue queue;
    std::vector<InputEvent> events;
    std::optional<ControlState> control;
    bool controlling = false;
    // Input has gone to the interface alone since this was last asked.
    bool kept = false;
    // Touches come from the system, and those of touch devices are left unread.
    bool system = false;

    // Routes what a device just reported: to the interface, and in control mode on to Android.
    void Dispatch(uint32_t index, InputDevice& device, std::vector<Sample>& samples) {
        kept |= device.grabbed && !samples.empty();
        const Utils::Overloaded route{
            [&](const Contact& contact) {
                if (!device.roles.touch || !device.screen || system) {
                    return;
                }
                const std::array<float, 2> position = device.roles.touch->ToScreen(contact.u, contact.v, view.size, view.rotation);
                const TouchTarget target = router.RouteContact(index, contact, position, view, events);
                const bool passes = target == TouchTarget::Outside && (device.grabbed || contact.phase != Phase::Start);
                if (control && passes) {
                    control->Send(Owner::Panel(index, contact.id), contact.phase, Panel::FromScreen(position, view.size, view.rotation));
                    kept = false;
                }
            },
            [&](const Sample::Hover& hover) { router.RouteHover(Pointer::ToScreen(hover.u, hover.v, view.size), view, events); },
            [&](const Sample::Button& button) { router.RouteButton(button.button, button.pressed, view, events); },
            [&](const Sample::Wheel& wheel) { router.RouteWheel(wheel.x, wheel.y, view, events); },
            [&](const Sample::Key& key) {
                if (!key.repeat) {
                    device.keysDown = key.pressed ? device.keysDown + 1 : device.keysDown - std::min(device.keysDown, 1u);
                }
                router.RouteKey(key.code, key.pressed, key.repeat, view, events);
            },
        };
        for (const Sample& sample : samples) {
            std::visit(route, sample.value);
        }
        samples.clear();
        const Clock::time_point now = Clock::now();
        for (InputEvent& event : events) {
            queue.Push(std::move(event), now);
        }
        events.clear();
    }
};

struct TouchReader::ReaderState {
    std::unordered_map<uint32_t, InputDevice> devices;
    std::vector<Sample> samples;
    RoutingState routing;
    // Every device that was there at the start has been looked at.
    bool scanned = false;

    std::optional<nanoseconds> Tick(nanoseconds now) {
        std::optional<nanoseconds> next;
        for (auto& [index, device] : devices) {
            if (const auto at = device.parser.Deadline(); at && now >= *at) {
                device.parser.Tick(now, samples);
                routing.Dispatch(index, device, samples);
            }
            next = Earlier(next, device.parser.Deadline());
        }
        SyncControl(now);
        const bool settling = routing.control && routing.controlling && now < routing.control->ready;
        return Earlier(next, settling ? std::optional(routing.control->ready) : std::nullopt);
    }

    // Brings every device's grab in line with the mode. A device changes hands only while
    // nothing on it is held down, so neither side is left with a stuck finger or key.
    void SyncControl(nanoseconds now) {
        if (!routing.control) {
            return;
        }
        ControlState& active = *routing.control;
        const bool ready = routing.controlling && now >= active.ready;
        bool grabbed = false;
        for (auto& [index, device] : devices) {
            // A screen whose touches come by way of the system has to stay with the system.
            const bool wanted = ready && !device.refused && ((device.screen && !routing.system) || (device.roles.keyboard && routing.view.wantsKeys));
            const bool settled = device.parser.IsIdle() && device.keysDown == 0 && !active.fingers.HoldsPanel(index);
            if (device.grabbed != wanted && settled) {
                if (const auto taken = device.Grab(wanted); !taken) {
                    logger.Warn("{} stays shared: {}", device.path, taken.error().message);
                    device.refused = true;
                }
            }
            grabbed |= device.grabbed;
        }
        if (!routing.controlling) {
            active.fingers.Lift([](const Owner& owner) { return owner.kind == Owner::Kind::Code; }, active.events);
            active.Flush();
            if (!grabbed) {
                routing.control.reset();
            }
        }
    }

    // Only surfaces known to be the screen take part; guesses are a fallback for systems
    // that describe none.
    void PickScreens() {
        std::optional<Confidence> best;
        for (const auto& [index, device] : devices) {
            if (device.roles.touch && (!best || *best < device.roles.touch->confidence)) {
                best = device.roles.touch->confidence;
            }
        }
        for (auto& [index, device] : devices) {
            device.screen = device.roles.touch && (device.roles.touch->confidence == Confidence::Certain() || device.roles.touch->confidence == best);
        }
    }

    // Picks up fingers that were already down when the device was opened or lost events.
    void Restore(uint32_t index, int file) {
        const auto found = devices.find(index);
        if (found == devices.end()) {
            return;
        }
        InputDevice& device = found->second;
        if (device.slots == 0 || !device.roles.touch || device.roles.touch->protocol != Protocol::Slots()) {
            return;
        }
        std::vector<Contact> contacts;
        if (TouchParser* parser = device.parser.TouchMut()) {
            parser->RestoreSlots(TouchReader::ReadAxisState(file, device.axes, device.slots), TouchReader::IsKeyDown(file, BtnTouch), contacts);
        }
        for (const Contact& contact : contacts) {
            samples.push_back(Sample{contact});
        }
        routing.Dispatch(index, device, samples);
    }

private:
    template <class T>
    static std::optional<T> Earlier(std::optional<T> left, std::optional<T> right) {
        if (!left) {
            return right;
        }
        if (!right) {
            return left;
        }
        return std::min(*left, *right);
    }
};

Idc TouchReader::LoadIdc(const Capabilities& device) {
    const std::vector<std::string> names = Idc::FileNames(device);
    for (const std::string_view directory : {"/product/usr/idc", "/system_ext/usr/idc", "/odm/usr/idc", "/vendor/usr/idc", "/system/usr/idc", "/data/system/devices/idc"}) {
        for (const std::string& name : names) {
            const std::string path = std::format("{}/{}", directory, name);
            if (const auto text = Utils::ReadFile(path)) {
                return Idc::Parse(*text);
            }
        }
    }
    return {};
}

std::optional<TouchReader::InputDevice> TouchReader::OpenDevice(const std::string& path, std::optional<std::string_view> display) {
    auto file = std::make_shared<Fd>(open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (!*file) {
        return std::nullopt;
    }
    const auto capabilities = TouchReader::ReadCapabilities(file->Get());
    if (!capabilities || capabilities->name == VirtualTouchscreen) {
        return std::nullopt;
    }
    const Roles roles = Roles::Classify(*capabilities, LoadIdc(*capabilities), display);
    if (roles.IsEmpty()) {
        return std::nullopt;
    }
    const auto slotAxis = capabilities->FindAxis(AbsMtSlot);
    std::vector<uint16_t> axes;
    for (size_t code = 0; code < capabilities->axes.size(); code++) {
        if (capabilities->axes[code]) {
            axes.push_back(static_cast<uint16_t>(code));
        }
    }
    return InputDevice{.path = path, .name = capabilities->name, .roles = roles, .parser = DeviceParser(*capabilities, roles), .slots = slotAxis ? std::min(static_cast<size_t>(std::max(slotAxis->max, -1) + 1), size_t{32}) : 0, .file = std::move(file), .axes = std::move(axes)};
}

class TouchReader::Hub final : public TouchInjector, public std::enable_shared_from_this<Hub> {
public:
    mutable std::mutex mutex;
    std::condition_variable wake;
    ReaderState state;
    // Asked once the input is closed: every thread here ends the next time it wakes up.
    std::stop_source stopping;
    const Clock::time_point started = Clock::now();

    nanoseconds Elapsed() const {
        return Clock::now() - started;
    }

    Result<> SetControl(bool enabled) override {
        const std::scoped_lock lock(mutex);
        const nanoseconds now = Elapsed();
        if (enabled && !state.routing.control) {
            auto device = CreateVirtualTouchscreen(VirtualTouchscreen, VirtualRange, VirtualSlots);
            if (!device) {
                return Err("cannot create a virtual touchscreen: {}", device.error().message);
            }
            // How long Android gets to pick up the virtual touchscreen before the real one is taken away.
            state.routing.control.emplace(ControlState{.device = std::move(*device), .fingers = {}, .ready = now + 500ms, .events = {}, .bytes = {}});
        }
        state.routing.controlling = enabled;
        state.SyncControl(now);
        wake.notify_one();
        return {};
    }

    bool IsControl() const override {
        const std::scoped_lock lock(mutex);
        return state.routing.controlling;
    }

    Result<> MoveFinger(uint32_t finger, Phase phase, std::array<float, 2> position) override {
        std::unique_lock lock(mutex);
        const nanoseconds settling = state.routing.control ? state.routing.control->ready - Elapsed() : nanoseconds::zero();
        if (settling > nanoseconds::zero()) {
            lock.unlock();
            std::this_thread::sleep_for(settling);
            lock.lock();
        }
        RoutingState& routing = state.routing;
        if (!routing.control || !routing.controlling) {
            return Err("touch control is off");
        }
        if (routing.control->Send(Owner::Code(finger), phase, Panel::FromScreen(position, routing.view.size, routing.view.rotation))) {
            return {};
        }
        if (phase == Phase::Start) {
            return Err("every touch slot is taken");
        }
        return Err("finger {} is not down", finger);
    }

    // Looks for input devices, at the start and whenever one appears or leaves.
    void Scan(std::optional<std::string> display) {
        std::unordered_map<std::string, std::pair<uint64_t, int64_t>> seen;
        uint32_t nextIndex = 0;
        // Set up before the first look, so a device that appears during it is not missed.
        std::optional<DirectoryWatch> watch = DirectoryWatch::Create(DeviceDirectories);
        for (;;) {
            for (const char* directory : DeviceDirectories) {
                DIR* entries = opendir(directory);
                if (!entries) {
                    continue;
                }
                while (const dirent* entry = readdir(entries)) {
                    const std::string name = entry->d_name;
                    const std::string path = std::format("{}/{}", directory, name);
                    struct stat meta {};
                    // Only real event devices, which the kernel numbers under its major 13: a pipe
                    // or socket under the same name would lose what is read from it.
                    if (!name.starts_with("event") || stat(path.c_str(), &meta) != 0 || !S_ISCHR(meta.st_mode) || ((meta.st_rdev >> 8) & 0xfff) != 13) {
                        continue;
                    }
                    const std::pair<uint64_t, int64_t> identity{meta.st_ino, meta.st_ctime};
                    if (const auto known = seen.find(name); known != seen.end() && known->second == identity) {
                        continue;
                    }
                    seen[name] = identity;
                    std::optional<InputDevice> device = OpenDevice(path, display);
                    if (!device) {
                        continue;
                    }
                    const uint32_t index = ++nextIndex;
                    const std::shared_ptr<Fd> file = device->file;
                    logger.Debug("input device {}: {} [{}]", name, device->name, device->roles.Describe(false, false));
                    {
                        const std::scoped_lock lock(mutex);
                        state.devices.emplace(index, std::move(*device));
                        state.PickScreens();
                        state.Restore(index, file->Get());
                        state.SyncControl(Elapsed());
                    }
                    std::thread([hub = shared_from_this(), index, file, name] {
                        pthread_setname_np(pthread_self(), ("input-" + name).substr(0, 15).c_str());
                        hub->ReadDevice(index, file->Get());
                    }).detach();
                }
                closedir(entries);
            }
            {
                const std::scoped_lock lock(mutex);
                state.scanned = true;
            }
            // Devices come and go rarely, and their directories tell when: no looking in between.
            const bool changed = watch && watch->Wait();
            if (stopping.stop_requested()) {
                return;
            }
            // A device that has just appeared gets a moment for its owner and mode to be set.
            // Where the directories cannot be watched, the devices are looked through every second.
            std::this_thread::sleep_for(changed ? Clock::duration(200ms) : Clock::duration(1s));
        }
    }

    // Reads one device until it goes away.
    void ReadDevice(uint32_t index, int file) {
        std::vector<uint8_t> bytes((TouchReader::TimeSize + 8) * 64);
        std::vector<RawEvent> events;
        bool dropped = false;
        for (;;) {
            const ssize_t length = read(file, bytes.data(), bytes.size());
            if (length < 0 && errno == EINTR) {
                continue;
            }
            if (length <= 0 || stopping.stop_requested()) {
                break;
            }
            const nanoseconds now = Elapsed();
            const std::scoped_lock lock(mutex);
            const auto found = state.devices.find(index);
            if (found == state.devices.end()) {
                return;
            }
            InputDevice& device = found->second;
            bool resync = false;
            events.clear();
            RawEvent::Decode(std::span(bytes.data(), static_cast<size_t>(length)), TouchReader::TimeSize, events);
            for (const RawEvent& event : events) {
                if (event.kind == EvSyn && event.code == SynDropped) {
                    dropped = true;
                    device.parser.Cancel(state.samples);
                } else if (!dropped) {
                    device.parser.Feed(event, now, state.samples);
                } else if (event.kind == EvSyn && event.code == SynReport) {
                    dropped = false;
                    resync = true;
                }
            }
            state.routing.Dispatch(index, device, state.samples);
            const bool waiting = device.parser.Deadline().has_value();
            if (resync) {
                state.Restore(index, file);
            }
            state.SyncControl(now);
            if (waiting || !state.routing.queue.IsEmpty()) {
                wake.notify_one();
            }
        }

        const std::scoped_lock lock(mutex);
        if (auto node = state.devices.extract(index)) {
            InputDevice& device = node.mapped();
            device.parser.Cancel(state.samples);
            state.routing.Dispatch(index, device, state.samples);
        }
        state.PickScreens();
        state.SyncControl(Elapsed());
        wake.notify_one();
    }
};

TouchReader TouchReader::Start(std::optional<std::string> display) {
    auto hub = std::make_shared<Hub>();
    std::thread([hub, display = std::move(display)]() mutable {
        pthread_setname_np(pthread_self(), "input-scan");
        hub->Scan(std::move(display));
    }).detach();
    return TouchReader(std::move(hub));
}

void TouchReader::SwapView(InterfaceView& view) const {
    const std::scoped_lock lock(hub_->mutex);
    std::swap(hub_->state.routing.view, view);
}

std::shared_ptr<TouchInjector> TouchReader::GetInjector() const {
    return hub_;
}

bool TouchReader::HasScreen(std::chrono::milliseconds patience) const {
    const Clock::time_point until = Clock::now() + patience;
    for (;;) {
        {
            const std::scoped_lock lock(hub_->mutex);
            if (std::ranges::any_of(hub_->state.devices, [](const auto& entry) { return entry.second.screen; })) {
                return true;
            }
            if (hub_->state.scanned || Clock::now() >= until) {
                return false;
            }
        }
        std::this_thread::sleep_for(20ms);
    }
}

std::function<void(const Motion&)> TouchReader::SystemTouches(bool enabled) const {
    {
        const std::scoped_lock lock(hub_->mutex);
        hub_->state.routing.system = enabled;
    }
    return [hub = hub_](const Motion& motion) {
        const std::scoped_lock lock(hub->mutex);
        RoutingState& routing = hub->state.routing;
        for (const auto& [contact, position] : motion.Contacts()) {
            // The device number touches reported by the system go under.
            routing.router.RouteContact(0, contact, position, routing.view, routing.events);
        }
        const Clock::time_point now = Clock::now();
        for (InputEvent& event : routing.events) {
            routing.queue.Push(std::move(event), now);
        }
        routing.events.clear();
        hub->wake.notify_one();
    };
}

bool TouchReader::Wait(nanoseconds timeout) const {
    const Clock::time_point until = Clock::now() + timeout;
    std::unique_lock lock(hub_->mutex);
    for (;;) {
        const nanoseconds now = hub_->Elapsed();
        const std::optional<nanoseconds> deadline = hub_->state.Tick(now);
        if (!hub_->state.routing.queue.IsEmpty()) {
            return true;
        }
        const nanoseconds remaining = until - Clock::now();
        if (remaining <= nanoseconds::zero()) {
            return false;
        }
        const nanoseconds pause = deadline ? std::min(remaining, std::max(*deadline - now, nanoseconds::zero())) : remaining;
        hub_->wake.wait_for(lock, std::max<nanoseconds>(pause, 1ms));
    }
}

Taken TouchReader::Drain(std::vector<InputEvent>& into) const {
    const std::scoped_lock lock(hub_->mutex);
    const auto at = hub_->state.routing.queue.TakeFrame(into);
    return {at, std::exchange(hub_->state.routing.kept, false)};
}

std::vector<std::string> TouchReader::Describe() const {
    const std::scoped_lock lock(hub_->mutex);
    std::vector<std::string> lines;
    for (const auto& [index, device] : hub_->state.devices) {
        lines.push_back(std::format("{} \"{}\": {}", device.path, device.name, device.roles.Describe(device.screen, device.grabbed)));
    }
    std::ranges::sort(lines);
    return lines;
}

void TouchReader::Stop() const {
    hub_->stopping.request_stop();
    // Whatever was taken from Android goes back to it.
    (void)hub_->SetControl(false);
}

// The frames' side of the input: it hands what the readers collected to Dear ImGui and to
// the keyboards, and tells the readers where the interface is.
class TouchSession final : public InputSource {
public:
    explicit TouchSession(TouchReader reader) : reader_(std::move(reader)) {
    }

    ~TouchSession() override {
        reader_.Stop();
    }

    bool Wait(std::chrono::nanoseconds timeout) override {
        return reader_.Wait(timeout);
    }

    void Rest() override {
        helper_.Rest();
        keyboard.Rest();
    }

    Wish TakeInput(double time, const DisplayInfo& display, float scale) override {
        events_.clear();
        if (reader_.Drain(events_).kept) {
            ReportActivity();
        }
        for (const InputEvent& event : events_) {
            const Utils::Overloaded hand{
                [&](const InputEvent::Key& key) { keyboard.OnKey(key.code, key.pressed, time); },
                [&](const InputEvent::KeyboardTouch& press) { keyboard.OnTouch(press.id, press.phase, ImVec2(press.position[0], press.position[1]), time); },
                [&](const auto&) { bridge_.Feed(keyboard.ToInterface(event)); },
            };
            std::visit(hand, event.value);
        }
        // A key held down types again after a while, which takes a frame of its own.
        const Wish wish{.settle = !events_.empty(), .due = keyboard.Tick(time)};
        keyboard.Arrange(ImGui::GetIO().DisplaySize, display.density * scale, scale);
        return wish;
    }

    bool AfterNewFrame() override {
        ImGuiBridge::Confine();
        return bridge_.Scroll();
    }

    void BeforeRender() override {
        keyboard.Paint();
    }

    bool AfterRender(const DisplayInfo& display) override {
        const bool typing = keyboard.Finish();
        PublishView(display);
        return typing;
    }

    // Tells the input threads where the interface is, so they can tell its touches from the rest.
    void PublishView(const DisplayInfo& display) {
        if (ImGui::GetCurrentContext()->FrameCountEnded > 0) {
            // How far around a window a touch still counts as the window's, in density-independent pixels.
            ImGuiBridge::Describe(view_, 8.0f * display.density);
            keyboard.Describe(view_);
        }
        view_.size = {static_cast<float>(display.width), static_cast<float>(display.height)};
        view_.rotation = display.rotation;
        reader_.SwapView(view_);
    }

    // Takes the touches from Android itself where no touchscreen can be read, as inside a
    // container, or where that was asked for.
    void WatchSystem(uint32_t api) {
        // The input devices are looked through before anything else happens, so that a touch
        // right after the start is not missed.
        const bool screen = reader_.HasScreen(5s);
        // Set to `system` to take touches from Android itself even where a touchscreen can be read.
        const char* source = std::getenv("ANDROID_IMGUI_TOUCH");
        const bool asked = source && std::string_view(source) == "system";
        // The system has had an input monitor since Android 10.
        if (api < 29 || (!asked && screen)) {
            return;
        }
        logger.Info("touches come from the system's input monitor");
        if (auto watching = Helper::WatchTouches(reader_.SystemTouches(true))) {
            helper_ = std::move(*watching);
        } else {
            logger.Warn("no touches from the system: {}", watching.error().message);
            reader_.SystemTouches(false);
        }
    }

    const TouchReader& GetReader() const {
        return reader_;
    }

    std::span<const InputEvent> GetEvents() const {
        return events_;
    }

private:
    // Input the interface kept to itself never reached the system, which would let the screen
    // time out under the user's fingers. The helper tells it they are there.
    void ReportActivity() {
        // Once a second is enough to keep the screen awake.
        if (Clock::now() - reported_ < 1s) {
            return;
        }
        reported_ = Clock::now();
        helper_.Activity();
    }

    TouchReader reader_;
    ImGuiBridge bridge_;
    Helper helper_{HelperUser::Root};
    InterfaceView view_;
    std::vector<InputEvent> events_;
    Clock::time_point reported_;
};

bool Touch::Init(Graphics& graphics, bool readOnly) {
    Close();
    const NativeWindow& window = graphics.GetWindow();
    const auto session = std::make_shared<TouchSession>(TouchReader::Start(window.GetDisplayId()));
    session->PublishView(window.GetDisplayInfo());
    session->WatchSystem(window.GetApiLevel());
    keyboard.ShareClipboard(std::make_unique<Helper>(HelperUser::Shell));
    graphics.SetInput(session);
    {
        const std::scoped_lock lock(mutex_);
        injector_ = session->GetReader().GetInjector();
    }
    session_ = session;
    return readOnly || SetControl(true);
}

void Touch::Close() {
    if (!session_) {
        return;
    }
    keyboard.ShareClipboard(nullptr);
    {
        const std::scoped_lock lock(mutex_);
        injector_.reset();
    }
    session_.reset();
}

std::span<const InputEvent> Touch::GetEvents() const {
    return session_ ? session_->GetEvents() : std::span<const InputEvent>();
}

std::vector<std::string> Touch::GetDevices() const {
    return session_ ? session_->GetReader().Describe() : std::vector<std::string>();
}
