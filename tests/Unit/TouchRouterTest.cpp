#include "Input/Touch/TouchRouter.h"

#include "Test.h"

#include <algorithm>
#include <chrono>
#include <vector>

class TouchRouterTest {
protected:
    using Position = std::array<float, 2>;
    using Events = std::vector<InputEvent>;

    static InterfaceView Screen() {
        InterfaceView view;
        view.size = {2400.0f, 1504.0f};
        view.regions = {Area{100.0f, 100.0f, 500.0f, 400.0f}};
        view.keyboard = Area{0.0f, 1000.0f, 2400.0f, 1504.0f};
        return view;
    }

    static Contact MakeContact(uint32_t id, Phase phase) {
        return {id, phase, 0.0f, 0.0f};
    }

    static Events Route(TouchRouter& router, uint32_t id, Phase phase, Position position, const InterfaceView& view) {
        Events out;
        router.RouteContact(0, MakeContact(id, phase), position, view, out);
        return out;
    }

    static Events Frame(EventQueue& queue) {
        Events events;
        queue.TakeFrame(events);
        return events;
    }

    static Events Part(const Events& events, size_t from, size_t to) {
        return Events(events.begin() + static_cast<std::ptrdiff_t>(from), events.begin() + static_cast<std::ptrdiff_t>(to));
    }
};

TEST(TouchRouterTest, "finger resting outside never reaches the interface") {
    const InterfaceView view = Screen();
    TouchRouter router;
    EXPECT(Route(router, 1, Phase::Start, {1500.0f, 700.0f}, view).empty());
    EXPECT(Route(router, 1, Phase::Move, {300.0f, 200.0f}, view).empty());

    const Events tap = Route(router, 2, Phase::Start, {300.0f, 200.0f}, view);
    EXPECT(tap == (Events{
                      {InputEvent::Touch{0, 2, Phase::Start, {300.0f, 200.0f}}},
                      {InputEvent::PointerMoved{{300.0f, 200.0f}}},
                      {InputEvent::PointerButton{{300.0f, 200.0f}, MouseButton::Left, true}},
                  }));
    const Events lift = Route(router, 2, Phase::End, {310.0f, 200.0f}, view);
    EXPECT_EQ(lift.size(), size_t{3});
    if (lift.size() == 3) {
        EXPECT(lift[1] == (InputEvent{InputEvent::PointerButton{{310.0f, 200.0f}, MouseButton::Left, false}}));
        EXPECT(lift[2] == (InputEvent{InputEvent::PointerGone{}}));
    }
    EXPECT(Route(router, 1, Phase::End, {300.0f, 200.0f}, view).empty());
}

TEST(TouchRouterTest, "a drag keeps its finger outside the window") {
    const InterfaceView view = Screen();
    TouchRouter router;
    Route(router, 1, Phase::Start, {300.0f, 200.0f}, view);
    const Events moved = Route(router, 1, Phase::Move, {2000.0f, 900.0f}, view);
    EXPECT(moved.size() > 1 && moved[1] == (InputEvent{InputEvent::PointerMoved{{2000.0f, 900.0f}}}));

    const Events second = Route(router, 2, Phase::Start, {120.0f, 120.0f}, view);
    EXPECT(second == (Events{{InputEvent::Touch{0, 2, Phase::Start, {120.0f, 120.0f}}}}));
    const Events cancelled = Route(router, 1, Phase::Cancel, {2000.0f, 900.0f}, view);
    EXPECT(!cancelled.empty() && cancelled.back() == (InputEvent{InputEvent::PointerGone{}}));
    EXPECT(std::ranges::none_of(cancelled, [](const InputEvent& event) { return std::holds_alternative<InputEvent::PointerButton>(event.value); }));
}

TEST(TouchRouterTest, "keyboard touches stay away from the interface") {
    const InterfaceView view = Screen();
    TouchRouter router;
    const Events press = Route(router, 7, Phase::Start, {50.0f, 1200.0f}, view);
    EXPECT(press == (Events{{InputEvent::KeyboardTouch{7, Phase::Start, {50.0f, 1200.0f}}}}));
    const Events release = Route(router, 7, Phase::End, {50.0f, 1200.0f}, view);
    EXPECT(release == (Events{{InputEvent::KeyboardTouch{7, Phase::End, {50.0f, 1200.0f}}}}));
}

TEST(TouchRouterTest, "outside press dismisses only when asked") {
    InterfaceView view = Screen();
    TouchRouter router;
    EXPECT(Route(router, 1, Phase::Start, {1500.0f, 700.0f}, view).empty());
    view.dismissable = true;
    const Events dismissed = Route(router, 2, Phase::Start, {1500.0f, 700.0f}, view);
    EXPECT_EQ(dismissed.size(), size_t{4});
    EXPECT(dismissed.size() == 4 && dismissed[3] == (InputEvent{InputEvent::PointerGone{}}));
}

TEST(TouchRouterTest, "background counts once the root is used") {
    InterfaceView view;
    view.size = {1000.0f, 1000.0f};
    EXPECT(view.FindTarget({10.0f, 10.0f}) == TouchTarget::Outside);
    view.unused = Area{0.0f, 100.0f, 1000.0f, 1000.0f};
    EXPECT(view.FindTarget({10.0f, 10.0f}) == TouchTarget::Interface);
    EXPECT(view.FindTarget({10.0f, 500.0f}) == TouchTarget::Outside);
}

TEST(TouchRouterTest, "the wheel scrolls where the last touch was") {
    const InterfaceView view = Screen();
    TouchRouter router;
    Events out;
    for (const Phase phase : {Phase::Start, Phase::End}) {
        router.RouteContact(0, Contact{1, phase, 0.0f, 0.0f}, {300.0f, 200.0f}, view, out);
    }
    out.clear();
    router.RouteWheel(0.0f, -1.0f, view, out);
    router.RouteWheel(0.0f, -1.0f, view, out);
    EXPECT(out == (Events{{InputEvent::PointerMoved{{300.0f, 200.0f}}}, {InputEvent::Wheel{0.0f, -1.0f}}, {InputEvent::Wheel{0.0f, -1.0f}}}));
}

TEST(TouchRouterTest, "mouse hovers only over the interface") {
    const InterfaceView view = Screen();
    TouchRouter router;
    Events out;
    router.RouteHover({1500.0f, 700.0f}, view, out);
    router.RouteWheel(0.0f, 1.0f, view, out);
    EXPECT(out.empty());

    router.RouteHover({300.0f, 200.0f}, view, out);
    router.RouteWheel(0.0f, 1.0f, view, out);
    router.RouteButton(MouseButton::Left, true, view, out);
    router.RouteHover({1500.0f, 700.0f}, view, out);
    router.RouteButton(MouseButton::Left, false, view, out);
    router.RouteHover({1600.0f, 700.0f}, view, out);
    EXPECT(out == (Events{
                      {InputEvent::PointerMoved{{300.0f, 200.0f}}},
                      {InputEvent::Wheel{0.0f, 1.0f}},
                      {InputEvent::PointerMoved{{300.0f, 200.0f}}},
                      {InputEvent::PointerButton{{300.0f, 200.0f}, MouseButton::Left, true}},
                      {InputEvent::PointerMoved{{1500.0f, 700.0f}}},
                      {InputEvent::PointerButton{{1500.0f, 700.0f}, MouseButton::Left, false}},
                      {InputEvent::PointerGone{}},
                  }));
}

TEST(TouchRouterTest, "queue keeps the latest motion and every press") {
    EventQueue queue;
    const auto now = std::chrono::steady_clock::now();
    const auto touch = [](uint32_t id, Phase phase, float x) { return InputEvent{InputEvent::Touch{0, id, phase, {x, 0.0f}}}; };
    queue.Push(touch(1, Phase::Start, 0.0f), now);
    for (int step = 1; step <= 100; step++) {
        queue.Push(touch(1, Phase::Move, static_cast<float>(step)), now);
        queue.Push({InputEvent::PointerMoved{{static_cast<float>(step), 0.0f}}}, now);
        queue.Push(touch(2, Phase::Move, -static_cast<float>(step)), now);
    }
    queue.Push(touch(1, Phase::End, 100.0f), now);
    queue.Push(touch(1, Phase::Move, 5.0f), now);

    const Events events = Frame(queue);
    EXPECT(events == (Events{
                         touch(1, Phase::Start, 0.0f),
                         touch(1, Phase::Move, 100.0f),
                         {InputEvent::PointerMoved{{100.0f, 0.0f}}},
                         touch(2, Phase::Move, -100.0f),
                         touch(1, Phase::End, 100.0f),
                         touch(1, Phase::Move, 5.0f),
                     }));
}

TEST(TouchRouterTest, "frames are cut after a press and before a release that follows movement") {
    EventQueue queue;
    const auto now = std::chrono::steady_clock::now();
    const auto press = [](bool pressed) { return InputEvent{InputEvent::PointerButton{{10.0f, 10.0f}, MouseButton::Left, pressed}}; };
    const auto touch = [](Phase phase) { return InputEvent{InputEvent::Touch{0, 1, phase, {10.0f, 10.0f}}}; };
    const Events drag = {
        touch(Phase::Start), {InputEvent::PointerMoved{{10.0f, 10.0f}}}, press(true), touch(Phase::Move), {InputEvent::PointerMoved{{90.0f, 90.0f}}}, touch(Phase::End), press(false), {InputEvent::PointerGone{}},
    };
    for (const InputEvent& event : drag) {
        queue.Push(event, now);
    }
    EXPECT(Frame(queue) == Part(drag, 0, 3));
    EXPECT(Frame(queue) == Part(drag, 3, 5));
    EXPECT(Frame(queue) == Part(drag, 5, 8));

    const Events tap = {touch(Phase::Start), {InputEvent::PointerMoved{{10.0f, 10.0f}}}, press(true), touch(Phase::End), press(false), {InputEvent::PointerGone{}}};
    for (const InputEvent& event : tap) {
        queue.Push(event, now);
    }
    EXPECT(Frame(queue) == Part(tap, 0, 3));
    EXPECT(Frame(queue) == Part(tap, 3, 6));
    EXPECT(queue.IsEmpty());
}

TEST(TouchRouterTest, "a frame is as old as its latest event") {
    EventQueue queue;
    const auto start = std::chrono::steady_clock::now();
    const auto later = [start](int millis) { return start + std::chrono::milliseconds(millis); };
    const auto press = [](bool pressed) { return InputEvent{InputEvent::PointerButton{{10.0f, 10.0f}, MouseButton::Left, pressed}}; };
    queue.Push({InputEvent::PointerMoved{{10.0f, 10.0f}}}, later(0));
    queue.Push(press(true), later(5));
    queue.Push({InputEvent::PointerMoved{{20.0f, 10.0f}}}, later(30));
    queue.Push({InputEvent::PointerMoved{{30.0f, 10.0f}}}, later(60));
    queue.Push(press(false), later(70));

    Events events;
    EXPECT(queue.TakeFrame(events) == std::optional(later(5)));
    EXPECT(queue.TakeFrame(events) == std::optional(later(60)));
    EXPECT(queue.TakeFrame(events) == std::optional(later(70)));
    EXPECT(queue.TakeFrame(events) == std::nullopt);
}
