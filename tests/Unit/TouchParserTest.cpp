#include "Input/Touch/TouchParser.h"

#include "Test.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <tuple>
#include <utility>
#include <vector>

class TouchParserTest {
protected:
    using Codes = std::vector<std::pair<uint16_t, int32_t>>;
    using Seen = std::vector<std::tuple<uint32_t, Phase, std::array<float, 2>>>;

    static constexpr Owner PanelOwner = Owner::Panel(1, 7);

    static Codes CodesOf(const std::vector<RawEvent>& events) {
        Codes all;
        for (const RawEvent& event : events) {
            all.push_back({event.code, event.value});
        }
        return all;
    }

    static Seen PhasesOf(std::string_view line) {
        Seen seen;
        const std::optional<Motion> motion = Motion::Parse(line);
        EXPECT(motion.has_value());
        if (!motion) {
            return seen;
        }
        for (const auto& [contact, position] : motion->Contacts()) {
            seen.push_back({contact.id, contact.phase, position});
        }
        return seen;
    }

    using Phases = std::vector<std::pair<uint32_t, Phase>>;

    static constexpr std::chrono::nanoseconds Ms(int64_t count) {
        return std::chrono::milliseconds(count);
    }

    static Panel MakePanel(Protocol protocol, int32_t maxX, int32_t maxY) {
        return {
            .protocol = protocol,
            .x = {0, maxX},
            .y = {0, maxY},
            .orientation = Rotation::Deg0,
            .followsDisplay = true,
            .confidence = Confidence::Certain(),
            .button = false,
            .pressure = false,
        };
    }

    static Panel WithButton(Panel setup) {
        setup.button = true;
        return setup;
    }

    static Panel WithPressure(Panel setup) {
        setup.pressure = true;
        return setup;
    }

    static std::unique_ptr<TouchParser> ParserFor(Protocol protocol, int32_t maxX, int32_t maxY) {
        return TouchParser::Create(MakePanel(protocol, maxX, maxY));
    }

    static std::vector<Contact> Feed(TouchParser& parser, std::chrono::nanoseconds now, const std::vector<RawEvent>& events) {
        std::vector<Contact> out;
        for (const RawEvent& event : events) {
            parser.Feed(event, now, out);
        }
        return out;
    }

    static Phases PhasesOf(const std::vector<Contact>& contacts) {
        Phases seen;
        for (const Contact& contact : contacts) {
            seen.push_back({contact.id, contact.phase});
        }
        return seen;
    }

    static constexpr RawEvent SynEvent = {EvSyn, SynReport, 0};
    static constexpr RawEvent MtEvent = {EvSyn, SynMtReport, 0};

    static Panel MakePanel(Rotation orientation, bool followsDisplay) {
        return {
            .protocol = Protocol::Slots(),
            .x = {0, 15039},
            .y = {0, 23999},
            .orientation = orientation,
            .followsDisplay = followsDisplay,
            .confidence = Confidence::Certain(),
            .button = false,
            .pressure = false,
        };
    }

    static bool CloseTo(std::array<float, 2> actual, std::array<float, 2> expected) {
        return std::fabs(actual[0] - expected[0]) < 0.01f && std::fabs(actual[1] - expected[1]) < 0.01f;
    }
};

TEST(TouchParserTest, "a finger goes down moves and lifts") {
    Fingers fingers;
    std::vector<RawEvent> events;
    EXPECT(fingers.Apply(PanelOwner, Phase::Start, {0.5f, 0.25f}, events));
    EXPECT(CodesOf(events) == (Codes{{AbsMtSlot, 0}, {BtnTouch, 1}, {AbsMtTrackingId, 1}, {AbsMtPositionX, 16384}, {AbsMtPositionY, 8192}, {SynReport, 0}}));

    events.clear();
    EXPECT(fingers.Apply(PanelOwner, Phase::Move, {1.0f, -0.1f}, events));
    EXPECT(CodesOf(events) == (Codes{{AbsMtSlot, 0}, {AbsMtPositionX, VirtualRange - 1}, {AbsMtPositionY, 0}, {SynReport, 0}}));

    events.clear();
    EXPECT(fingers.Apply(PanelOwner, Phase::End, {0.0f, 0.0f}, events));
    EXPECT(CodesOf(events) == (Codes{{AbsMtSlot, 0}, {AbsMtTrackingId, -1}, {BtnTouch, 0}, {SynReport, 0}}));
    EXPECT(fingers.IsEmpty() && !fingers.Apply(PanelOwner, Phase::Move, {0.0f, 0.0f}, events));
}

TEST(TouchParserTest, "fingers share the screen without colliding") {
    Fingers fingers;
    std::vector<RawEvent> events;
    fingers.Apply(PanelOwner, Phase::Start, {0.1f, 0.1f}, events);
    events.clear();
    fingers.Apply(Owner::Code(0), Phase::Start, {0.2f, 0.2f}, events);
    const Codes second = CodesOf(events);
    EXPECT(second.size() >= 2 && second[0] == (std::pair{AbsMtSlot, int32_t{1}}) && second[1] == (std::pair{AbsMtTrackingId, int32_t{2}}));
    EXPECT(fingers.HoldsPanel(1) && !fingers.HoldsPanel(2));

    events.clear();
    fingers.Apply(PanelOwner, Phase::Cancel, {0.0f, 0.0f}, events);
    EXPECT(CodesOf(events) == (Codes{{AbsMtSlot, 0}, {AbsMtTrackingId, -1}, {SynReport, 0}}));
    fingers.Apply(Owner::Code(5), Phase::Start, {0.3f, 0.3f}, events);
    events.clear();
    fingers.Lift([](Owner owner) { return owner.kind == Owner::Kind::Code; }, events);
    const Codes lifted = CodesOf(events);
    EXPECT(fingers.IsEmpty() && std::ranges::find(lifted, std::pair{BtnTouch, int32_t{0}}) != lifted.end());
}

TEST(TouchParserTest, "the seventeenth finger is refused") {
    Fingers fingers;
    std::vector<RawEvent> events;
    for (uint32_t finger = 0; finger < VirtualSlots; finger++) {
        EXPECT(fingers.Apply(Owner::Code(finger), Phase::Start, {0.5f, 0.5f}, events));
    }
    EXPECT(!fingers.Apply(Owner::Code(99), Phase::Start, {0.5f, 0.5f}, events));
}

TEST(TouchParserTest, "lines from the helper become contacts") {
    EXPECT(PhasesOf("touch 0 0 3 120.5 640.0") == (Seen{{3, Phase::Start, {120.5f, 640.0f}}}));
    EXPECT(PhasesOf("touch 5 1 3 121.0 640.0 4 900.0 200.25") == (Seen{{3, Phase::Move, {121.0f, 640.0f}}, {4, Phase::Start, {900.0f, 200.25f}}}));
    EXPECT(PhasesOf("touch 2 0 3 130.0 650.0 4 905.0 210.0") == (Seen{{3, Phase::Move, {130.0f, 650.0f}}, {4, Phase::Move, {905.0f, 210.0f}}}));
    EXPECT(PhasesOf("touch 6 0 3 130.0 650.0 4 905.0 210.0") == (Seen{{3, Phase::End, {130.0f, 650.0f}}, {4, Phase::Move, {905.0f, 210.0f}}}));
    EXPECT(PhasesOf("touch 3 0 4 905.0 210.0") == (Seen{{4, Phase::Cancel, {905.0f, 210.0f}}}));
    EXPECT(PhasesOf("touch 7 0 4 905.0 210.0").empty());
    EXPECT(!Motion::Parse("ready") && !Motion::Parse("touch 0 0 3 12.0"));
}

TEST(TouchParserTest, "slots follow one finger") {
    const std::unique_ptr<TouchParser> made = ParserFor(Protocol::Slots(), 15039, 23999);
    TouchParser& parser = *made;
    const std::vector<Contact> down = Feed(parser, Ms(0), {{EvAbs, AbsMtSlot, 0}, {EvAbs, AbsMtTrackingId, 77}, {EvAbs, AbsMtPositionX, 7520}, {EvAbs, AbsMtPositionY, 12000}, {EvKey, BtnTouch, 1}, SynEvent});
    EXPECT(PhasesOf(down) == (Phases{{1, Phase::Start}}));
    EXPECT(down.size() == 1 && std::fabs(down[0].u - 0.5f) < 1e-4f && std::fabs(down[0].v - 0.5f) < 1e-4f);

    const std::vector<Contact> moved = Feed(parser, Ms(8), {{EvAbs, AbsMtPositionX, 15039}, SynEvent});
    EXPECT(PhasesOf(moved) == (Phases{{1, Phase::Move}}));
    EXPECT(moved.size() == 1 && moved[0].u > 0.9999f && moved[0].u < 1.0f);

    EXPECT(Feed(parser, Ms(16), {SynEvent}).empty());
    const std::vector<Contact> up = Feed(parser, Ms(24), {{EvAbs, AbsMtTrackingId, -1}, {EvKey, BtnTouch, 0}, SynEvent});
    EXPECT(PhasesOf(up) == (Phases{{1, Phase::End}}));
}

TEST(TouchParserTest, "slots track fingers independently") {
    const std::unique_ptr<TouchParser> made = ParserFor(Protocol::Slots(), 1000, 1000);
    TouchParser& parser = *made;
    Feed(parser, Ms(0), {{EvAbs, AbsMtSlot, 0}, {EvAbs, AbsMtTrackingId, 1}, {EvAbs, AbsMtPositionX, 100}, {EvAbs, AbsMtPositionY, 100}, SynEvent});
    const std::vector<Contact> second = Feed(parser, Ms(5), {{EvAbs, AbsMtSlot, 1}, {EvAbs, AbsMtTrackingId, 2}, {EvAbs, AbsMtPositionX, 800}, {EvAbs, AbsMtPositionY, 800}, SynEvent});
    EXPECT(PhasesOf(second) == (Phases{{2, Phase::Start}}));

    const std::vector<Contact> lifted = Feed(parser, Ms(10), {{EvAbs, AbsMtSlot, 0}, {EvAbs, AbsMtTrackingId, -1}, SynEvent});
    EXPECT(PhasesOf(lifted) == (Phases{{1, Phase::End}}));

    const std::vector<Contact> replaced = Feed(parser, Ms(15), {{EvAbs, AbsMtSlot, 1}, {EvAbs, AbsMtTrackingId, 9}, SynEvent});
    EXPECT(PhasesOf(replaced) == (Phases{{2, Phase::End}, {3, Phase::Start}}));
}

TEST(TouchParserTest, "restored slots report fingers already down") {
    const std::unique_ptr<TouchParser> made = ParserFor(Protocol::Slots(), 1000, 1000);
    TouchParser& parser = *made;
    std::vector<Contact> out;
    AxisState held;
    held.slot = 1;
    held.values = {{AbsMtTrackingId, {-1, 5}}, {AbsMtPositionX, {0, 500}}, {AbsMtPositionY, {0, 250}}, {AbsMtPressure, {0, 30}}};
    parser.RestoreSlots(held, false, out);
    EXPECT(PhasesOf(out) == (Phases{{1, Phase::Start}}));
    EXPECT(out.size() == 1 && std::fabs(out[0].u - 0.4995f) < 1e-3f);

    const std::unique_ptr<TouchParser> madePressing = TouchParser::Create(WithPressure(WithButton(MakePanel(Protocol::Slots(), 1000, 1000))));
    TouchParser& pressing = *madePressing;
    std::vector<Contact> restored;
    pressing.RestoreSlots(held, false, restored);
    EXPECT(restored.empty() && pressing.IsIdle());
    pressing.RestoreSlots(held, true, restored);
    EXPECT(PhasesOf(restored) == (Phases{{1, Phase::Start}}));

    out.clear();
    parser.Cancel(out);
    EXPECT(PhasesOf(out) == (Phases{{1, Phase::Cancel}}));
}

TEST(TouchParserTest, "frames release a tap after a moment") {
    const std::unique_ptr<TouchParser> made = ParserFor(Protocol::Frames(false), 32767, 32767);
    TouchParser& parser = *made;
    const std::vector<Contact> down = Feed(parser, Ms(0), {{EvAbs, AbsMtPositionX, 16384}, {EvAbs, AbsMtPositionY, 8192}, MtEvent, SynEvent});
    EXPECT(PhasesOf(down) == (Phases{{1, Phase::Start}}));
    EXPECT(Feed(parser, Ms(1), {{EvAbs, AbsMtPositionX, 16384}, {EvAbs, AbsMtPositionY, 8192}, MtEvent, SynEvent}).empty());

    EXPECT(Feed(parser, Ms(66), {MtEvent, SynEvent}).empty());
    EXPECT(Feed(parser, Ms(80), {MtEvent, SynEvent}).empty());
    EXPECT(parser.Deadline() == std::optional(Ms(106)));
    std::vector<Contact> out;
    parser.Tick(Ms(106), out);
    EXPECT(PhasesOf(out) == (Phases{{1, Phase::End}}));
    EXPECT(parser.Deadline() == std::nullopt);
}

TEST(TouchParserTest, "frames keep a fresh contact through stray empty frames") {
    const std::unique_ptr<TouchParser> made = ParserFor(Protocol::Frames(false), 32767, 32767);
    TouchParser& parser = *made;
    std::vector<Contact> seen;
    const auto see = [&seen](const std::vector<Contact>& contacts) { seen.insert(seen.end(), contacts.begin(), contacts.end()); };
    for (int32_t step = 0; step < 20; step++) {
        const int32_t x = 1000 + step * 500;
        see(Feed(parser, Ms(step * 12), {{EvAbs, AbsMtPositionX, x}, {EvAbs, AbsMtPositionY, 1000}, MtEvent, SynEvent}));
        for (const int32_t noise : {1, 4, 7, 10}) {
            see(Feed(parser, Ms(step * 12 + noise), {MtEvent, SynEvent}));
        }
    }
    const Phases all = PhasesOf(seen);
    EXPECT(!all.empty() && all[0] == (std::pair{uint32_t{1}, Phase::Start}));
    EXPECT(std::ranges::all_of(all.begin() + (all.empty() ? 0 : 1), all.end(), [](const auto& phase) { return phase == std::pair{uint32_t{1}, Phase::Move}; }));
}

TEST(TouchParserTest, "frames start afresh once a release is overdue") {
    const std::unique_ptr<TouchParser> made = ParserFor(Protocol::Frames(false), 32767, 32767);
    TouchParser& parser = *made;
    Feed(parser, Ms(0), {{EvAbs, AbsMtPositionX, 1000}, {EvAbs, AbsMtPositionY, 1000}, MtEvent, SynEvent});
    Feed(parser, Ms(50), {MtEvent, SynEvent});
    const std::vector<Contact> next = Feed(parser, Ms(150), {{EvAbs, AbsMtPositionX, 1010}, {EvAbs, AbsMtPositionY, 1000}, MtEvent, SynEvent});
    EXPECT(PhasesOf(next) == (Phases{{1, Phase::End}, {2, Phase::Start}}));
}

TEST(TouchParserTest, "frames ride out a dropped contact") {
    const std::unique_ptr<TouchParser> made = ParserFor(Protocol::Frames(false), 32767, 32767);
    TouchParser& parser = *made;
    Feed(parser, Ms(0), {{EvAbs, AbsMtPositionX, 1000}, {EvAbs, AbsMtPositionY, 1000}, MtEvent, SynEvent});
    Feed(parser, Ms(300), {{EvAbs, AbsMtPositionX, 2000}, {EvAbs, AbsMtPositionY, 1000}, MtEvent, SynEvent});

    EXPECT(Feed(parser, Ms(310), {MtEvent, SynEvent}).empty());
    EXPECT(parser.Deadline() == std::optional(Ms(560)));

    std::vector<Contact> out;
    parser.Tick(Ms(400), out);
    EXPECT(out.empty());

    const std::vector<Contact> resumed = Feed(parser, Ms(450), {{EvAbs, AbsMtPositionX, 2500}, {EvAbs, AbsMtPositionY, 1000}, MtEvent, SynEvent});
    EXPECT(PhasesOf(resumed) == (Phases{{1, Phase::Move}}));

    EXPECT(Feed(parser, Ms(460), {MtEvent, SynEvent}).empty());
    parser.Tick(Ms(709), out);
    EXPECT(out.empty());
    parser.Tick(Ms(710), out);
    EXPECT(PhasesOf(out) == (Phases{{1, Phase::End}}));
}

TEST(TouchParserTest, "frames release when the panel goes silent") {
    const std::unique_ptr<TouchParser> made = ParserFor(Protocol::Frames(false), 32767, 32767);
    TouchParser& parser = *made;
    Feed(parser, Ms(0), {{EvAbs, AbsMtPositionX, 1000}, {EvAbs, AbsMtPositionY, 1000}, MtEvent, SynEvent});
    EXPECT(parser.Deadline() == std::optional(Ms(1000)));
    std::vector<Contact> out;
    parser.Tick(Ms(1000), out);
    EXPECT(PhasesOf(out) == (Phases{{1, Phase::End}}));
}

TEST(TouchParserTest, "frames match untracked contacts by distance") {
    const std::unique_ptr<TouchParser> made = TouchParser::Create(WithButton(MakePanel(Protocol::Frames(false), 1000, 1000)));
    TouchParser& parser = *made;
    const std::vector<Contact> down = Feed(parser, Ms(0), {{EvKey, BtnTouch, 1}, {EvAbs, AbsMtPositionX, 100}, {EvAbs, AbsMtPositionY, 100}, MtEvent, {EvAbs, AbsMtPositionX, 900}, {EvAbs, AbsMtPositionY, 900}, MtEvent, SynEvent});
    EXPECT(PhasesOf(down) == (Phases{{1, Phase::Start}, {2, Phase::Start}}));

    const std::vector<Contact> swapped = Feed(parser, Ms(10), {{EvAbs, AbsMtPositionX, 880}, {EvAbs, AbsMtPositionY, 900}, MtEvent, {EvAbs, AbsMtPositionX, 120}, {EvAbs, AbsMtPositionY, 100}, MtEvent, SynEvent});
    EXPECT(PhasesOf(swapped) == (Phases{{2, Phase::Move}, {1, Phase::Move}}));

    const std::vector<Contact> oneLeft = Feed(parser, Ms(20), {{EvAbs, AbsMtPositionX, 880}, {EvAbs, AbsMtPositionY, 900}, MtEvent, SynEvent});
    EXPECT(PhasesOf(oneLeft) == (Phases{{1, Phase::End}}));

    const std::vector<Contact> up = Feed(parser, Ms(30), {{EvKey, BtnTouch, 0}, MtEvent, SynEvent});
    EXPECT(PhasesOf(up) == (Phases{{2, Phase::End}}));
}

TEST(TouchParserTest, "a finger hovers until the touch button goes down") {
    const std::unique_ptr<TouchParser> made = TouchParser::Create(WithButton(MakePanel(Protocol::Slots(), 1000, 1000)));
    TouchParser& parser = *made;
    const std::vector<Contact> nearby = Feed(parser, Ms(0), {{EvAbs, AbsMtSlot, 0}, {EvAbs, AbsMtTrackingId, 3}, {EvAbs, AbsMtPositionX, 400}, {EvAbs, AbsMtPositionY, 400}, SynEvent});
    EXPECT(nearby.empty() && parser.IsIdle());
    EXPECT(PhasesOf(Feed(parser, Ms(10), {{EvKey, BtnTouch, 1}, SynEvent})) == (Phases{{1, Phase::Start}}));
    EXPECT(PhasesOf(Feed(parser, Ms(20), {{EvAbs, AbsMtPositionX, 500}, SynEvent})) == (Phases{{1, Phase::Move}}));
    EXPECT(PhasesOf(Feed(parser, Ms(30), {{EvKey, BtnTouch, 0}, SynEvent})) == (Phases{{1, Phase::End}}));
    EXPECT(Feed(parser, Ms(40), {{EvAbs, AbsMtPositionX, 600}, SynEvent}).empty());
    EXPECT(PhasesOf(Feed(parser, Ms(50), {{EvKey, BtnTouch, 1}, SynEvent})) == (Phases{{2, Phase::Start}}));
}

TEST(TouchParserTest, "slots a driver left at zero are not fingers") {
    const std::unique_ptr<TouchParser> byPressure = TouchParser::Create(WithPressure(MakePanel(Protocol::Slots(), 1000, 1000)));
    AxisState state;
    state.values = {{AbsMtTrackingId, {0, 0, 0}}, {AbsMtPressure, {0, 0, 0}}, {AbsMtToolType, {0, 0, 0}}};
    EXPECT(!byPressure->IsTouching(state, false));
    state.values[1].second[2] = 400;
    EXPECT(byPressure->IsTouching(state, false));
    state.values[2].second[2] = MtToolPalm;
    EXPECT(!byPressure->IsTouching(state, false));

    const std::unique_ptr<TouchParser> byButton = TouchParser::Create(WithButton(MakePanel(Protocol::Slots(), 1000, 1000)));
    AxisState held;
    held.values = {{AbsMtTrackingId, {-1, 7, -1}}};
    EXPECT(!byButton->IsTouching(held, false) && byButton->IsTouching(held, true));
    EXPECT(!byButton->IsTouching(AxisState{}, true));
}

TEST(TouchParserTest, "a finger without pressure or marked as a palm is not a touch") {
    const std::unique_ptr<TouchParser> made = TouchParser::Create(WithPressure(MakePanel(Protocol::Slots(), 1000, 1000)));
    TouchParser& parser = *made;
    const std::vector<Contact> light = Feed(parser, Ms(0), {{EvAbs, AbsMtSlot, 0}, {EvAbs, AbsMtTrackingId, 3}, {EvAbs, AbsMtPositionX, 400}, {EvAbs, AbsMtPositionY, 400}, SynEvent});
    EXPECT(light.empty());
    EXPECT(PhasesOf(Feed(parser, Ms(10), {{EvAbs, AbsMtPressure, 50}, SynEvent})) == (Phases{{1, Phase::Start}}));
    EXPECT(PhasesOf(Feed(parser, Ms(20), {{EvAbs, AbsMtToolType, MtToolPalm}, SynEvent})) == (Phases{{1, Phase::End}}));
    EXPECT(PhasesOf(Feed(parser, Ms(30), {{EvAbs, AbsMtToolType, 0}, SynEvent})) == (Phases{{2, Phase::Start}}));
    EXPECT(PhasesOf(Feed(parser, Ms(40), {{EvAbs, AbsMtPressure, 0}, {EvAbs, AbsMtTrackingId, -1}, SynEvent})) == (Phases{{2, Phase::End}}));
}

TEST(TouchParserTest, "frames with a touch button release at once and ignore stray positions") {
    const std::unique_ptr<TouchParser> made = TouchParser::Create(WithButton(MakePanel(Protocol::Frames(false), 1080, 1920)));
    TouchParser& parser = *made;
    const std::vector<Contact> down = Feed(parser, Ms(0), {{EvKey, BtnTouch, 1}, {EvAbs, AbsMtPressure, 1}, {EvAbs, AbsMtPositionX, 540}, {EvAbs, AbsMtPositionY, 960}, MtEvent, SynEvent});
    EXPECT(PhasesOf(down) == (Phases{{1, Phase::Start}}));
    const std::vector<Contact> up = Feed(parser, Ms(30), {{EvKey, BtnTouch, 0}, {EvAbs, AbsMtPressure, 0}, {EvAbs, AbsMtPositionX, 540}, {EvAbs, AbsMtPositionY, 960}, MtEvent, SynEvent});
    EXPECT(PhasesOf(up) == (Phases{{1, Phase::End}}));
    EXPECT(parser.Deadline() == std::nullopt);
    EXPECT(Feed(parser, Ms(60), {{EvAbs, AbsMtPositionX, 100}, {EvAbs, AbsMtPositionY, 100}, {EvRel, RelWheel, 1}, SynEvent}).empty());
}

TEST(TouchParserTest, "every slot of a wide panel is its own finger") {
    const std::unique_ptr<TouchParser> made = ParserFor(Protocol::Slots(), 1080, 1920);
    TouchParser& parser = *made;
    std::vector<Contact> seen;
    for (const int32_t slot : {0, 15, 16, 31}) {
        const std::vector<Contact> contacts = Feed(parser, Ms(0), {{EvAbs, AbsMtSlot, slot}, {EvAbs, AbsMtTrackingId, slot + 100}, {EvAbs, AbsMtPositionX, slot * 10}, {EvAbs, AbsMtPositionY, 500}, SynEvent});
        seen.insert(seen.end(), contacts.begin(), contacts.end());
    }
    EXPECT(PhasesOf(seen) == (Phases{{1, Phase::Start}, {2, Phase::Start}, {3, Phase::Start}, {4, Phase::Start}}));
}

TEST(TouchParserTest, "single touch uses the touch button") {
    const std::unique_ptr<TouchParser> made = ParserFor(Protocol::Single(), 4095, 4095);
    TouchParser& parser = *made;
    const std::vector<Contact> down = Feed(parser, Ms(0), {{EvAbs, AbsX, 2048}, {EvAbs, AbsY, 1024}, {EvKey, BtnTouch, 1}, SynEvent});
    EXPECT(PhasesOf(down) == (Phases{{1, Phase::Start}}));
    EXPECT(down.size() == 1 && std::fabs(down[0].u - 0.5f) < 1e-3f && std::fabs(down[0].v - 0.25f) < 1e-3f);
    EXPECT(PhasesOf(Feed(parser, Ms(5), {{EvAbs, AbsX, 3000}, SynEvent})) == (Phases{{1, Phase::Move}}));
    EXPECT(PhasesOf(Feed(parser, Ms(9), {{EvKey, BtnTouch, 0}, SynEvent})) == (Phases{{1, Phase::End}}));
}

TEST(TouchParserTest, "follows every display rotation") {
    const Panel touch = MakePanel(Rotation::Deg0, true);
    const float u = 0.25f, v = 0.1f;
    EXPECT(CloseTo(touch.ToScreen(u, v, {1504.0f, 2400.0f}, Rotation::Deg0), {376.0f, 240.0f}));
    EXPECT(CloseTo(touch.ToScreen(u, v, {2400.0f, 1504.0f}, Rotation::Deg90), {240.0f, 1128.0f}));
    EXPECT(CloseTo(touch.ToScreen(u, v, {1504.0f, 2400.0f}, Rotation::Deg180), {1128.0f, 2160.0f}));
    EXPECT(CloseTo(touch.ToScreen(u, v, {2400.0f, 1504.0f}, Rotation::Deg270), {2160.0f, 376.0f}));
}

TEST(TouchParserTest, "screen positions map back to the panel") {
    const Panel touch = MakePanel(Rotation::Deg0, true);
    const std::pair<Rotation, std::array<float, 2>> cases[] = {
        {Rotation::Deg0, {1504.0f, 2400.0f}},
        {Rotation::Deg90, {2400.0f, 1504.0f}},
        {Rotation::Deg180, {1504.0f, 2400.0f}},
        {Rotation::Deg270, {2400.0f, 1504.0f}},
    };
    for (const auto& [rotation, size] : cases) {
        const auto [u, v] = Panel::FromScreen(touch.ToScreen(0.25f, 0.1f, size, rotation), size, rotation);
        EXPECT(std::fabs(u - 0.25f) < 1e-5f && std::fabs(v - 0.1f) < 1e-5f);
    }
}

TEST(TouchParserTest, "four quarter turns are the identity") {
    float u = 0.3f, v = 0.8f;
    for (int turn = 0; turn < 4; turn++) {
        std::tie(u, v) = Panel::Rotate(u, v, Rotation::Deg90);
    }
    EXPECT(std::fabs(u - 0.3f) < 1e-6f && std::fabs(v - 0.8f) < 1e-6f);
    const auto [a, b] = Panel::Rotate(0.3f, 0.8f, Rotation::Deg90);
    const auto [c, d] = Panel::Rotate(a, b, Rotation::Deg270);
    EXPECT(std::fabs(c - 0.3f) < 1e-6f && std::fabs(d - 0.8f) < 1e-6f);
}

TEST(TouchParserTest, "panel orientation and fixed panels") {
    const Panel turned = MakePanel(Rotation::Deg90, true);
    EXPECT(CloseTo(turned.ToScreen(0.25f, 0.1f, {1000.0f, 500.0f}, Rotation::Deg0), {100.0f, 375.0f}));

    const Panel fixed = MakePanel(Rotation::Deg0, false);
    EXPECT(CloseTo(fixed.ToScreen(0.25f, 0.1f, {1000.0f, 500.0f}, Rotation::Deg270), {250.0f, 50.0f}));
    EXPECT(CloseTo(Pointer::ToScreen(0.5f, 0.5f, {2560.0f, 1440.0f}), {1280.0f, 720.0f}));
}
