#include "Input/Touch/TouchRouter.h"

#include <algorithm>
#include <limits>
#include <span>
#include <utility>

constexpr uint64_t MouseKeyboardId = std::numeric_limits<uint64_t>::max();

bool InputEvent::IsButton(bool pressed) const {
    const auto* button = std::get_if<InputEvent::PointerButton>(&value);
    return button && button->pressed == pressed;
}

bool InputEvent::IsLift() const {
    const auto* finger = std::get_if<InputEvent::Touch>(&value);
    return finger && (finger->phase == Phase::End || finger->phase == Phase::Cancel);
}

bool InputEvent::IsPointerMotion() const {
    return std::holds_alternative<InputEvent::PointerMoved>(value);
}

bool InputEvent::IsMotion() const {
    if (const auto* finger = std::get_if<InputEvent::Touch>(&value)) {
        return finger->phase == Phase::Move;
    }
    if (const auto* finger = std::get_if<InputEvent::KeyboardTouch>(&value)) {
        return finger->phase == Phase::Move;
    }
    return IsPointerMotion();
}

bool InputEvent::IsSameMotion(const InputEvent& event) const {
    if (!IsMotion() || !event.IsMotion()) {
        return false;
    }
    const auto* finger = std::get_if<InputEvent::Touch>(&value);
    const auto* otherFinger = std::get_if<InputEvent::Touch>(&event.value);
    if (finger && otherFinger) {
        return finger->device == otherFinger->device && finger->id == otherFinger->id;
    }
    const auto* key = std::get_if<InputEvent::KeyboardTouch>(&value);
    const auto* otherKey = std::get_if<InputEvent::KeyboardTouch>(&event.value);
    if (key && otherKey) {
        return key->id == otherKey->id;
    }
    return IsPointerMotion() && event.IsPointerMotion();
}

TouchTarget InterfaceView::FindTarget(std::array<float, 2> position) const {
    if (keyboard && keyboard->Contains(position)) {
        return TouchTarget::Keyboard;
    }
    const bool inside = std::ranges::any_of(regions, [&position](const Area& area) { return area.Contains(position); });
    if (inside || (unused && !unused->Contains(position))) {
        return TouchTarget::Interface;
    }
    return TouchTarget::Outside;
}

TouchTarget TouchRouter::RouteContact(uint32_t device, Contact contact, std::array<float, 2> position, const InterfaceView& view, std::vector<InputEvent>& out) {
    const uint64_t finger = uint64_t{device} << 32 | uint64_t{contact.id};
    const auto route = routes_.find(finger);
    const std::optional<TouchTarget> known = route == routes_.end() ? std::nullopt : std::optional(route->second);
    const TouchTarget target = contact.phase == Phase::Start ? view.FindTarget(position) : known.value_or(TouchTarget::Outside);
    switch (contact.phase) {
        case Phase::Start:
            routes_.insert_or_assign(finger, target);
            switch (target) {
                case TouchTarget::Interface:
                    out.push_back({InputEvent::Touch{device, contact.id, Phase::Start, position}});
                    if (!owner_ && !IsDragging()) {
                        owner_ = finger;
                        mouse_ = position;
                        out.push_back({InputEvent::PointerMoved{position}});
                        out.push_back({InputEvent::PointerButton{position, MouseButton::Left, true}});
                    }
                    break;
                case TouchTarget::Keyboard:
                    out.push_back({InputEvent::KeyboardTouch{finger, Phase::Start, position}});
                    break;
                case TouchTarget::Outside:
                    Dismiss(position, view, out);
                    break;
            }
            break;
        case Phase::Move:
            if (known == TouchTarget::Interface) {
                out.push_back({InputEvent::Touch{device, contact.id, Phase::Move, position}});
                if (owner_ == finger) {
                    mouse_ = position;
                    out.push_back({InputEvent::PointerMoved{position}});
                }
            } else if (known == TouchTarget::Keyboard) {
                out.push_back({InputEvent::KeyboardTouch{finger, Phase::Move, position}});
            }
            break;
        case Phase::End:
        case Phase::Cancel:
            if (route != routes_.end()) {
                routes_.erase(route);
            }
            if (known == TouchTarget::Interface) {
                out.push_back({InputEvent::Touch{device, contact.id, contact.phase, position}});
                if (owner_ == finger) {
                    owner_.reset();
                    hovering_ = false;
                    if (contact.phase == Phase::End) {
                        out.push_back({InputEvent::PointerButton{position, MouseButton::Left, false}});
                    }
                    out.push_back({InputEvent::PointerGone{}});
                }
            } else if (known == TouchTarget::Keyboard) {
                out.push_back({InputEvent::KeyboardTouch{finger, contact.phase, position}});
            }
            break;
    }
    return target;
}

void TouchRouter::RouteHover(std::array<float, 2> position, const InterfaceView& view, std::vector<InputEvent>& out) {
    mouse_ = position;
    if (owner_) {
        return;
    }
    if (typing_) {
        out.push_back({InputEvent::KeyboardTouch{MouseKeyboardId, Phase::Move, position}});
    } else if (IsDragging() || view.FindTarget(position) == TouchTarget::Interface) {
        hovering_ = true;
        out.push_back({InputEvent::PointerMoved{position}});
    } else if (std::exchange(hovering_, false)) {
        out.push_back({InputEvent::PointerGone{}});
    }
}

void TouchRouter::RouteButton(MouseButton which, bool pressed, const InterfaceView& view, std::vector<InputEvent>& out) {
    const std::array<float, 2> position = mouse_;
    const size_t index = static_cast<size_t>(which);
    if (!pressed) {
        if (std::exchange(held_[index], false)) {
            out.push_back({InputEvent::PointerButton{position, which, false}});
        } else if (which == MouseButton::Left && std::exchange(typing_, false)) {
            out.push_back({InputEvent::KeyboardTouch{MouseKeyboardId, Phase::End, position}});
        }
        return;
    }
    if (owner_) {
        return;
    }
    switch (view.FindTarget(position)) {
        case TouchTarget::Interface:
            held_[index] = true;
            hovering_ = true;
            out.push_back({InputEvent::PointerMoved{position}});
            out.push_back({InputEvent::PointerButton{position, which, true}});
            break;
        case TouchTarget::Keyboard:
            if (which == MouseButton::Left) {
                typing_ = true;
                out.push_back({InputEvent::KeyboardTouch{MouseKeyboardId, Phase::Start, position}});
            }
            break;
        case TouchTarget::Outside:
            if (which == MouseButton::Left) {
                Dismiss(position, view, out);
            }
            break;
    }
}

void TouchRouter::RouteWheel(float x, float y, const InterfaceView& view, std::vector<InputEvent>& out) {
    if (view.FindTarget(mouse_) == TouchTarget::Interface) {
        if (!owner_ && !std::exchange(hovering_, true)) {
            out.push_back({InputEvent::PointerMoved{mouse_}});
        }
        out.push_back({InputEvent::Wheel{x, y}});
    }
}

void TouchRouter::RouteKey(uint16_t code, bool pressed, bool repeat, const InterfaceView& view, std::vector<InputEvent>& out) {
    if (view.wantsKeys || !pressed) {
        out.push_back({InputEvent::Key{code, pressed, repeat}});
    }
}

bool TouchRouter::IsDragging() const {
    return std::ranges::any_of(held_, [](bool held) { return held; });
}

void TouchRouter::Dismiss(std::array<float, 2> position, const InterfaceView& view, std::vector<InputEvent>& out) {
    if (view.dismissable && !owner_ && !IsDragging()) {
        out.push_back({InputEvent::PointerMoved{position}});
        out.push_back({InputEvent::PointerButton{position, MouseButton::Left, true}});
        out.push_back({InputEvent::PointerButton{position, MouseButton::Left, false}});
        out.push_back({InputEvent::PointerGone{}});
        hovering_ = false;
    }
}

bool EventQueue::IsEmpty() const {
    return events_.empty();
}

std::optional<std::chrono::steady_clock::time_point> EventQueue::TakeFrame(std::vector<InputEvent>& into) {
    const auto first = [this](size_t end, const auto& wanted) -> std::optional<size_t> {
        const auto frame = std::span(events_).first(end);
        const auto found = std::ranges::find_if(frame, wanted);
        if (found == frame.end()) {
            return std::nullopt;
        }
        return static_cast<size_t>(found - frame.begin());
    };
    const std::optional<size_t> press = first(events_.size(), [](const InputEvent& event) { return event.IsButton(true); });
    size_t end = press ? *press + 1 : events_.size();
    if (std::optional<size_t> release = first(end, [](const InputEvent& event) { return event.IsButton(false); })) {
        if (*release > 0 && events_[*release - 1].IsLift()) {
            *release -= 1;
        }
        if (first(*release, &InputEvent::IsPointerMotion)) {
            end = *release;
        }
    }
    const auto taken = static_cast<std::ptrdiff_t>(end);
    into.insert(into.end(), events_.begin(), events_.begin() + taken);
    events_.erase(events_.begin(), events_.begin() + taken);
    std::optional<std::chrono::steady_clock::time_point> latest;
    for (size_t index = 0; index < end; index++) {
        latest = latest ? std::max(*latest, times_[index]) : times_[index];
    }
    times_.erase(times_.begin(), times_.begin() + taken);
    return latest;
}

void EventQueue::Push(InputEvent event, std::chrono::steady_clock::time_point at) {
    if (event.IsMotion()) {
        for (size_t index = events_.size(); index > 0 && events_[index - 1].IsMotion(); index--) {
            if (events_[index - 1].IsSameMotion(event)) {
                events_[index - 1] = event;
                times_[index - 1] = at;
                return;
            }
        }
    }
    events_.push_back(event);
    times_.push_back(at);
}
