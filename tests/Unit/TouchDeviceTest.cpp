#include "Input/Touch/TouchDevice.h"

#include "Input/Touch/TouchParser.h"
#include "Test.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <initializer_list>
#include <string>
#include <vector>

class TouchDeviceTest {
protected:
    // The kernel's side of a slotted device: what it holds, and the slot it last announced.
    struct Kernel {
        AxisState state;
        int32_t addressing = 0;

        void Write(const std::vector<RawEvent>& events) {
            for (const RawEvent& event : events) {
                if (event.kind != EvAbs) {
                    continue;
                }
                if (event.code == AbsMtSlot) {
                    addressing = event.value;
                    continue;
                }
                const bool slotted = event.code >= AbsMtFirst && event.code <= AbsMtLast;
                const auto column = std::ranges::find_if(state.values, [&event](const auto& axis) { return axis.first == event.code; });
                const size_t place = slotted ? static_cast<size_t>(addressing) : 0;
                const bool known = column != state.values.end() && place < column->second.size();
                EXPECT(known);
                if (!known) {
                    continue;
                }
                int32_t& held = column->second[place];
                if (held != event.value) {
                    held = event.value;
                    if (slotted) {
                        state.slot = addressing;
                    }
                }
            }
        }
    };

    static AxisState State(int32_t slot, std::array<int32_t, 3> x, std::array<int32_t, 3> y, int32_t single) {
        AxisState made;
        made.slot = slot;
        made.values = {{AbsX, {single}}, {AbsMtPositionX, std::vector<int32_t>(x.begin(), x.end())}, {AbsMtPositionY, std::vector<int32_t>(y.begin(), y.end())}, {AbsMtTrackingId, {-1, -1, -1}}};
        return made;
    }

    static constexpr uint32_t DirectProperty = 1u << PropDirect;
    static constexpr std::string_view ScreenIdc = "touch.deviceType = touchScreen\ntouch.orientationAware = 1\n";
    static inline const std::vector<uint16_t> KeyboardKeys = {1, KeyEnter, KeyA, 57};
    static inline const std::vector<uint16_t> MouseKeys = {BtnLeft, BtnRight, BtnMiddle};
    static constexpr uint16_t BtnToolFinger = 0x145;

    struct Range {
        uint16_t code;
        int32_t min;
        int32_t max;
    };

    static Capabilities Device(std::string_view name, uint32_t properties, const std::vector<uint16_t>& keys, const std::vector<Range>& axes, const std::vector<uint16_t>& relative) {
        Capabilities made;
        made.name = name;
        made.properties = properties;
        made.keys.assign(KeyCount / 8, 0);
        made.axes.assign(AbsCount, std::nullopt);
        for (const uint16_t key : keys) {
            made.keys[key / 8] |= static_cast<uint8_t>(1u << (key % 8));
        }
        for (const Range& axis : axes) {
            made.axes[axis.code] = Axis{axis.min, axis.max};
        }
        for (const uint16_t code : relative) {
            made.relative |= 1u << code;
        }
        return made;
    }

    static std::vector<uint16_t> Together(std::initializer_list<std::vector<uint16_t>> lists) {
        std::vector<uint16_t> all;
        for (const std::vector<uint16_t>& list : lists) {
            all.insert(all.end(), list.begin(), list.end());
        }
        return all;
    }

    static std::vector<Range> Slots(int32_t count, int32_t width, int32_t height) {
        return {{AbsMtSlot, 0, count - 1}, {AbsMtPositionX, 0, width}, {AbsMtPositionY, 0, height}, {AbsMtTrackingId, 0, 65535}};
    }

    static Panel Screen(const Roles& roles) {
        EXPECT(roles.touch.has_value());
        if (!roles.touch) {
            return {};
        }
        EXPECT(!roles.pointer && roles.touch->confidence == Confidence::Certain());
        return *roles.touch;
    }

    static Capabilities Blank() {
        Capabilities device;
        device.keys.assign(KeyCount / 8, 0);
        device.axes.assign(AbsCount, std::nullopt);
        return device;
    }

    static DeviceParser Mouse() {
        Capabilities device = Blank();
        for (const uint16_t key : {BtnLeft, BtnRight, BtnMiddle}) {
            device.keys[key / 8] |= static_cast<uint8_t>(1u << (key % 8));
        }
        device.axes[AbsX] = Axis{0, 65535};
        device.axes[AbsY] = Axis{0, 65535};
        device.relative = 1u << RelWheel | 1u << RelHwheel;
        return DeviceParser(device, Roles::Classify(device, Idc{}, std::nullopt));
    }

    static std::vector<Sample> Feed(DeviceParser& parser, const std::vector<RawEvent>& events) {
        std::vector<Sample> out;
        for (const RawEvent& event : events) {
            parser.Feed(event, std::chrono::nanoseconds::zero(), out);
        }
        return out;
    }
};

TEST(TouchDeviceTest, "events survive encoding") {
    const std::vector<RawEvent> events = {{EvAbs, AbsMtPositionX, -7}, {EvSyn, SynReport, 0}};
    for (const size_t timeSize : {size_t{8}, size_t{16}}) {
        std::vector<uint8_t> bytes;
        for (const RawEvent& event : events) {
            event.Encode(timeSize, bytes);
        }
        std::vector<RawEvent> decoded;
        RawEvent::Decode(bytes, timeSize, decoded);
        EXPECT(decoded == events);
    }
}

TEST(TouchDeviceTest, "rewinding puts every axis and the slot back") {
    const AxisState earlier = State(1, {100, 200, 300}, {10, 20, 30}, 5);
    const std::array<AxisState, 4> cases = {
        State(1, {100, 200, 300}, {10, 20, 30}, 5),
        State(0, {100, 200, 300}, {10, 20, 30}, 5),
        State(2, {100, 999, 300}, {10, 20, 31}, 9),
        State(1, {7, 8, 9}, {1, 2, 3}, 5),
    };
    for (const AxisState& now : cases) {
        std::vector<RawEvent> events;
        now.Rewind(earlier, events);
        const bool untouched = now == earlier;
        EXPECT_EQ(events.empty(), untouched);
        EXPECT(untouched || (!events.empty() && events.back() == (RawEvent{EvSyn, SynReport, 0})));
        EXPECT(std::ranges::none_of(events, [](const RawEvent& event) { return event.code == AbsMtTrackingId; }));

        Kernel kernel{now, now.slot};
        kernel.Write(events);
        EXPECT(kernel.state == earlier);
    }
}

TEST(TouchDeviceTest, "tablet panel is a slot touchscreen") {
    std::vector<Range> axes = Slots(10, 15039, 23999);
    axes.insert(axes.end(), {{AbsX, 0, 15039}, {AbsY, 0, 23999}});
    const Panel panel = Screen(Roles::Classify(Device("synaptics_tcm_touch", DirectProperty, {BtnTouch, BtnToolFinger, 143}, axes, {}), Idc{}, std::nullopt));
    EXPECT(panel.protocol == Protocol::Slots());
    EXPECT_EQ(panel.x.max, 15039);
    EXPECT_EQ(panel.y.max, 23999);
    EXPECT_EQ(panel.button, true);
    EXPECT_EQ(panel.pressure, false);
}

TEST(TouchDeviceTest, "bluestacks panels need their idc file") {
    const Capabilities frames = Device("BlueStacks Virtual Touch", 0, {}, {{AbsMtPositionX, 0, 32767}, {AbsMtPositionY, 0, 32767}}, {});
    EXPECT(Idc::FileNames(frames) == (std::vector<std::string>{"BlueStacks_Virtual_Touch.idc"}));
    const Idc idc = Idc::Parse("# Basic Parameters\ntouch.deviceType = touchScreen\n#touch.orientationAware = 1\ndevice.internal = 1\n");
    const Panel panel = Screen(Roles::Classify(frames, idc, std::nullopt));
    EXPECT(panel.protocol == Protocol::Frames(false));
    EXPECT_EQ(panel.button, false);
    const Roles guessed = Roles::Classify(frames, Idc{}, std::nullopt);
    EXPECT(guessed.touch && guessed.touch->confidence == Confidence::Guess(32767));

    const Capabilities newer = Device("BlueStacks Virtual Touch", 0, {BtnLeft, BtnRight, BtnMiddle, BtnTouch}, Slots(16, 32767, 32767), {});
    EXPECT(Screen(Roles::Classify(newer, idc, std::nullopt)).protocol == Protocol::Slots());
}

TEST(TouchDeviceTest, "android emulator nodes belong to one display each") {
    std::vector<Range> axes = Slots(11, 32767, 32767);
    axes.insert(axes.end(), {{AbsX, 0, 32767}, {AbsY, 0, 32767}, {AbsMtPressure, 0, 1024}, {AbsMtToolType, 0, 15}});
    const Capabilities node = Device("virtio_input_multi_touch_1", 0, {0x141, 0x14b}, axes, {});
    const Idc idc = Idc::Parse(std::string(ScreenIdc) + "touch.displayId = local:4619827259155550721\n");

    const Panel panel = Screen(Roles::Classify(node, idc, "local:4619827259155550721"));
    EXPECT(panel.protocol == Protocol::Slots());
    EXPECT_EQ(panel.button, false);
    EXPECT_EQ(panel.pressure, true);
    EXPECT(!Roles::Classify(node, idc, "local:4619827551948147201").touch);
    EXPECT(Roles::Classify(node, idc, std::nullopt).touch.has_value());
}

TEST(TouchDeviceTest, "emulator panels are told apart by what they report") {
    const Capabilities genymotion = Device("Genymotion Virtual Input", 0, Together({KeyboardKeys, {BtnTouch, BtnLeft, BtnToolFinger}}), {{AbsMtPositionX, 0, 1080}, {AbsMtPositionY, 0, 1920}, {AbsMtPressure, 0, 0}}, {RelWheel, RelHwheel});
    Roles roles = Roles::Classify(genymotion, Idc::Parse(ScreenIdc), std::nullopt);
    EXPECT(Screen(roles).protocol == Protocol::Frames(false));
    EXPECT_EQ(Screen(roles).button, true);
    EXPECT_EQ(Screen(roles).pressure, false);
    EXPECT(roles.keyboard && roles.wheel);

    std::vector<Range> axes = Slots(16, 1279, 719);
    axes.push_back({AbsMtPressure, 0, 2});
    const Capabilities ldplayer = Device("input", DirectProperty, Together({KeyboardKeys, {BtnTouch, BtnToolFinger}}), axes, {RelWheel, RelHwheel});
    roles = Roles::Classify(ldplayer, Idc{}, std::nullopt);
    EXPECT(Screen(roles).protocol == Protocol::Slots());
    EXPECT_EQ(Screen(roles).pressure, true);
    EXPECT(roles.keyboard && roles.wheel);

    const Capabilities nox = Device("Android_Input", 0, {}, {{AbsMtPositionX, 0, 1280}, {AbsMtPositionY, 0, 720}, {AbsMtTrackingId, 0, 1}}, {});
    EXPECT(Screen(Roles::Classify(nox, Idc::Parse(ScreenIdc), std::nullopt)).protocol == Protocol::Frames(true));

    axes = Slots(32, 1080, 1920);
    axes.push_back({AbsMtPressure, 0, 0});
    const Capabilities mumu = Device("Xiaomi Input", DirectProperty, Together({KeyboardKeys, MouseKeys, {BtnTouch, BtnToolFinger}}), axes, {0, 1, RelWheel, RelHwheel});
    roles = Roles::Classify(mumu, Idc{}, std::nullopt);
    EXPECT(Screen(roles).protocol == Protocol::Slots());
    EXPECT_EQ(Screen(roles).pressure, false);
    EXPECT(roles.keyboard && roles.wheel);

    const Capabilities memu = Device("User Input", DirectProperty, {BtnTouch}, {{AbsMtSlot, 0, 9}, {AbsMtPositionX, 0, 1280}, {AbsMtPositionY, 0, 720}, {AbsMtTrackingId, 0, 65535}, {0x30, 0, 255}}, {});
    EXPECT(Screen(Roles::Classify(memu, Idc{}, std::nullopt)).protocol == Protocol::Slots());

    const Capabilities oneSlot = Device("panel", DirectProperty, {BtnTouch}, {{AbsMtSlot, 0, 0}, {AbsMtPositionX, 0, 1280}, {AbsMtPositionY, 0, 720}, {AbsMtTrackingId, 0, 65535}}, {});
    EXPECT(Screen(Roles::Classify(oneSlot, Idc{}, std::nullopt)).protocol == Protocol::Frames(true));
}

TEST(TouchDeviceTest, "absolute mice are pointers even when they list a touch button") {
    const std::vector<Capabilities> tablets = {
        Device("QEMU QEMU USB Tablet", 0, MouseKeys, {{AbsX, 0, 32767}, {AbsY, 0, 32767}}, {RelWheel}),
        Device("QEMU Virtio Tablet", 0, Together({MouseKeys, {BtnTouch}}), {{AbsX, 0, 32767}, {AbsY, 0, 32767}}, {RelWheel}),
        Device("VirtualBox mouse integration", 0, MouseKeys, {{AbsX, 0, 65535}, {AbsY, 0, 65535}}, {RelWheel, RelHwheel}),
    };
    for (const Capabilities& tablet : tablets) {
        const Roles roles = Roles::Classify(tablet, Idc{}, std::nullopt);
        EXPECT(!roles.touch && roles.pointer && roles.wheel && !roles.keyboard);
    }

    const Capabilities resistive = Device("ADS7846 Touchscreen", DirectProperty, {BtnTouch}, {{AbsX, 0, 4095}, {AbsY, 0, 4095}, {AbsPressure, 0, 255}}, {});
    const Panel panel = Screen(Roles::Classify(resistive, Idc{}, std::nullopt));
    EXPECT(panel.protocol == Protocol::Single());
    EXPECT_EQ(panel.button, true);
    EXPECT_EQ(panel.pressure, true);
}

TEST(TouchDeviceTest, "keyboards sensors touchpads and gamepads are not screens") {
    const Capabilities keyboard = Device("AT Translated Set 2 keyboard", 0, KeyboardKeys, {}, {});
    const Roles roles = Roles::Classify(keyboard, Idc{}, std::nullopt);
    EXPECT(roles.keyboard && !roles.touch && !roles.pointer);

    const Capabilities fingerprint = Device("goodix_fp", 0, {102, 103}, {{AbsMtPositionX, 0, 1000}, {AbsMtPositionY, 0, 1000}}, {});
    EXPECT(!Roles::Classify(fingerprint, Idc{}, std::nullopt).touch);

    const Capabilities touchpad = Device("Synaptics TouchPad", 1u << PropPointer, {BtnTouch}, Slots(5, 4000, 3000), {});
    EXPECT(!Roles::Classify(touchpad, Idc{}, std::nullopt).touch);

    const Capabilities gamepad = Device("Xiaomi Joystick", 0, {BtnJoystick, 0x130, BtnLeft}, {{AbsX, -1'000'000, 1'000'000}, {AbsY, -1'000'000, 1'000'000}}, {0, 1});
    EXPECT(Roles::Classify(gamepad, Idc{}, std::nullopt).IsEmpty());

    const Capabilities flat = Device("broken", DirectProperty, {BtnTouch}, {{AbsMtPositionX, 0, 0}, {AbsMtPositionY, 0, 0}}, {});
    EXPECT(!Roles::Classify(flat, Idc{}, std::nullopt).touch);
}

TEST(TouchDeviceTest, "idc orientation and names") {
    const Idc idc = Idc::Parse("touch.deviceType = touchScreen\ntouch.orientation = ORIENTATION_270\ntouch.orientationAware = 0\n");
    EXPECT(idc.orientation == Rotation::Deg270);
    EXPECT(idc.orientationAware == false);

    Capabilities usb;
    usb.name = "Some Panel v2";
    usb.vendor = 0x04f3;
    usb.product = 0x261a;
    usb.version = 0x0100;
    EXPECT(Idc::FileNames(usb) == (std::vector<std::string>{"Vendor_04f3_Product_261a_Version_0100.idc", "Vendor_04f3_Product_261a.idc", "Some_Panel_v2.idc"}));
}

TEST(TouchDeviceTest, "mouse reports position before the click") {
    DeviceParser parser = Mouse();
    const std::vector<Sample> samples = Feed(parser, {{EvKey, BtnLeft, 1}, {EvAbs, AbsX, 32768}, {EvAbs, AbsY, 16384}, {EvSyn, SynReport, 0}});
    EXPECT(samples == (std::vector<Sample>{{Sample::Hover{0.5f, 0.25f}}, {Sample::Button{MouseButton::Left, true}}}));
}

TEST(TouchDeviceTest, "wheel steps accumulate per frame") {
    DeviceParser parser = Mouse();
    const std::vector<Sample> samples = Feed(parser, {{EvRel, RelWheel, 1}, {EvRel, RelWheel, 1}, {EvRel, RelHwheel, -1}, {EvSyn, SynReport, 0}});
    EXPECT(samples == (std::vector<Sample>{{Sample::Wheel{-1.0f, 2.0f}}}));
}

TEST(TouchDeviceTest, "keyboard ignores mouse buttons") {
    Capabilities device = Blank();
    device.keys[30 / 8] |= static_cast<uint8_t>(1u << (30 % 8));
    DeviceParser parser(device, Roles::Classify(device, Idc{}, std::nullopt));
    const std::vector<Sample> samples = Feed(parser, {{EvKey, 30, 1}, {EvKey, BtnLeft, 1}, {EvKey, 30, 2}, {EvKey, 30, 0}});
    EXPECT(samples == (std::vector<Sample>{{Sample::Key{30, true, false}}, {Sample::Key{30, true, true}}, {Sample::Key{30, false, false}}}));
}
