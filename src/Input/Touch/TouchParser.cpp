#include "Input/Touch/TouchParser.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <span>
#include <string>
#include <tuple>
#include <utility>

constexpr uint32_t ActionDown = 0;
constexpr uint32_t ActionUp = 1;
constexpr uint32_t ActionMove = 2;
constexpr uint32_t ActionCancel = 3;
constexpr uint32_t PointerDown = 5;
constexpr uint32_t PointerUp = 6;

bool Fingers::IsEmpty() const {
    return std::ranges::none_of(owners_, [](const std::optional<Owner>& owner) { return owner.has_value(); });
}

bool Fingers::HoldsPanel(uint32_t wanted) const {
    return std::ranges::any_of(owners_, [wanted](const std::optional<Owner>& owner) { return owner && owner->kind == Owner::Kind::Panel && owner->device == wanted; });
}

bool Fingers::Apply(Owner owner, Phase phase, std::pair<float, float> at, std::vector<RawEvent>& out) {
    const auto position = [this](const auto& wanted) -> std::optional<size_t> {
        const auto found = std::ranges::find_if(owners_, wanted);
        if (found == owners_.end()) {
            return std::nullopt;
        }
        return static_cast<size_t>(found - owners_.begin());
    };
    const std::optional<size_t> held = position([&owner](const std::optional<Owner>& slot) { return slot == owner; });
    const bool lifting = phase == Phase::End || phase == Phase::Cancel;
    std::optional<size_t> slot = held;
    if (!slot && phase == Phase::Start) {
        slot = position([](const std::optional<Owner>& free) { return !free; });
    }
    if (!slot) {
        return false;
    }
    const auto absolute = [&out](uint16_t code, int32_t value) { out.push_back({EvAbs, code, value}); };
    const auto button = [&out](int32_t value) { out.push_back({EvKey, BtnTouch, value}); };

    absolute(AbsMtSlot, static_cast<int32_t>(*slot));
    if (lifting) {
        owners_[*slot].reset();
        absolute(AbsMtTrackingId, -1);
        if (IsEmpty()) {
            button(0);
        }
    } else {
        if (!held) {
            if (IsEmpty()) {
                button(1);
            }
            owners_[*slot] = owner;
            tracking_ = (tracking_ + 1) & 0xffff;
            absolute(AbsMtTrackingId, tracking_);
        }
        const auto unit = [](float fraction) {
            const float units = fraction * static_cast<float>(VirtualRange);
            // Converting a number an `int32_t` cannot hold, or one that is not a number, is undefined.
            if (!(units > 0.0f)) {
                return 0;
            }
            return units >= static_cast<float>(VirtualRange - 1) ? VirtualRange - 1 : static_cast<int32_t>(units);
        };
        absolute(AbsMtPositionX, unit(at.first));
        absolute(AbsMtPositionY, unit(at.second));
    }
    out.push_back({EvSyn, SynReport, 0});
    return true;
}

std::optional<std::string_view> Motion::NextField(std::string_view& line) {
    while (!line.empty() && Utils::IsBlank(line.front())) {
        line.remove_prefix(1);
    }
    if (line.empty()) {
        return std::nullopt;
    }
    const size_t length = static_cast<size_t>(std::ranges::find_if(line, Utils::IsBlank) - line.begin());
    const std::string_view field = line.substr(0, length);
    line.remove_prefix(length);
    return field;
}

template <std::unsigned_integral T>
std::optional<T> Motion::ParseUnsigned(std::string_view text) {
    if (text.starts_with('+')) {
        text.remove_prefix(1);
    }
    return Utils::ParseNumber<T>(text);
}

std::optional<float> Motion::ParseJavaFloat(std::string_view text) {
    std::string_view rest = text;
    const bool negative = rest.starts_with('-');
    if (negative || rest.starts_with('+')) {
        rest.remove_prefix(1);
    }
    const float sign = negative ? -1.0f : 1.0f;
    if (Utils::EqualsIgnoreCase(rest, "inf") || Utils::EqualsIgnoreCase(rest, "infinity")) {
        return std::copysign(std::numeric_limits<float>::infinity(), sign);
    }
    if (Utils::EqualsIgnoreCase(rest, "nan")) {
        return std::copysign(std::numeric_limits<float>::quiet_NaN(), sign);
    }

    const auto digits = [&rest] {
        const size_t before = rest.size();
        while (!rest.empty() && Utils::IsDigit(rest.front())) {
            rest.remove_prefix(1);
        }
        return before - rest.size();
    };
    size_t mantissa = digits();
    if (rest.starts_with('.')) {
        rest.remove_prefix(1);
        mantissa += digits();
    }
    if (mantissa == 0) {
        return std::nullopt;
    }
    if (rest.starts_with('e') || rest.starts_with('E')) {
        rest.remove_prefix(1);
        if (rest.starts_with('-') || rest.starts_with('+')) {
            rest.remove_prefix(1);
        }
        if (digits() == 0) {
            return std::nullopt;
        }
    }
    if (!rest.empty()) {
        return std::nullopt;
    }
    return std::strtof(std::string(text).c_str(), nullptr);
}

std::optional<Motion> Motion::Parse(std::string_view line) {
    constexpr std::string_view MotionPrefix = "touch ";
    if (!line.starts_with(MotionPrefix)) {
        return std::nullopt;
    }
    line.remove_prefix(MotionPrefix.size());

    Motion motion;
    const std::optional<uint32_t> what = NextField(line).and_then(ParseUnsigned<uint32_t>);
    const std::optional<size_t> which = what ? NextField(line).and_then(ParseUnsigned<size_t>) : std::nullopt;
    if (!which) {
        return std::nullopt;
    }
    motion.action = *what;
    motion.index = *which;
    while (const std::optional<std::string_view> field = NextField(line)) {
        const std::optional<uint32_t> id = ParseUnsigned<uint32_t>(*field);
        const std::optional<float> x = id ? NextField(line).and_then(ParseJavaFloat) : std::nullopt;
        const std::optional<float> y = x ? NextField(line).and_then(ParseJavaFloat) : std::nullopt;
        if (!y) {
            return std::nullopt;
        }
        motion.pointers.push_back({*id, {*x, *y}});
    }
    return motion;
}

std::vector<std::pair<Contact, std::array<float, 2>>> Motion::Contacts() const {
    std::vector<std::pair<Contact, std::array<float, 2>>> meant;
    const bool down = action == ActionDown || action == PointerDown;
    const bool up = action == ActionUp || action == PointerUp;
    for (size_t finger = 0; finger < pointers.size(); finger++) {
        Phase phase;
        if (down && finger == index) {
            phase = Phase::Start;
        } else if (up && finger == index) {
            phase = Phase::End;
        } else if (down || up || action == ActionMove) {
            phase = Phase::Move;
        } else if (action == ActionCancel) {
            phase = Phase::Cancel;
        } else {
            continue;
        }
        meant.push_back({Contact{pointers[finger].first, phase, 0.0f, 0.0f}, pointers[finger].second});
    }
    return meant;
}

using std::chrono::nanoseconds;

TouchParser::TouchParser(const Panel& setup) : setup_(setup) {
}

std::unique_ptr<TouchParser> TouchParser::Create(const Panel& setup) {
    switch (setup.protocol.kind) {
        case Protocol::Kind::Slots:
            return std::make_unique<SlotsParser>(setup);
        case Protocol::Kind::Frames:
            return std::make_unique<FramesParser>(setup);
        case Protocol::Kind::Single:
            break;
    }
    return std::make_unique<SingleParser>(setup);
}

std::optional<nanoseconds> TouchParser::Deadline() const {
    return std::nullopt;
}

void TouchParser::Tick(nanoseconds, std::vector<Contact>&) {
}

void TouchParser::RestoreSlots(const AxisState&, bool, std::vector<Contact>&) {
}

bool TouchParser::Hovers(bool pressed, int32_t pressure, bool palm) const {
    return (setup_.button && !pressed) || (setup_.pressure && pressure <= 0) || palm;
}

bool TouchParser::IsTouching(const AxisState& state, bool pressed) const {
    const std::span<const int32_t> tracking = state.Column(AbsMtTrackingId);
    const std::span<const int32_t> pressure = state.Column(AbsMtPressure);
    const std::span<const int32_t> tool = state.Column(AbsMtToolType);
    for (size_t slot = 0; slot < tracking.size(); slot++) {
        if (tracking[slot] >= 0 && !Hovers(pressed, ValueAt(pressure, slot), ValueAt(tool, slot) == MtToolPalm)) {
            return true;
        }
    }
    return false;
}

Contact TouchParser::MakeContact(uint32_t id, Phase phase, const Point& point) const {
    return {id, phase, setup_.x.Normalize(point.x), setup_.y.Normalize(point.y)};
}

uint32_t TouchParser::FreshId() {
    nextId_++;
    return nextId_;
}

int32_t TouchParser::ValueAt(std::span<const int32_t> values, size_t slot) {
    return slot < values.size() ? values[slot] : 0;
}

SlotsParser::SlotsParser(const Panel& setup) : TouchParser(setup) {
}

void SlotsParser::Feed(RawEvent event, nanoseconds, std::vector<Contact>& out) {
    if (event.kind == EvAbs) {
        switch (event.code) {
            case AbsMtSlot:
                current_ = static_cast<size_t>(std::clamp(event.value, 0, static_cast<int32_t>(MaxContacts) - 1));
                break;
            case AbsMtTrackingId:
                CurrentSlot().tracking = event.value;
                break;
            case AbsMtPressure:
                CurrentSlot().pressure = event.value;
                break;
            case AbsMtToolType:
                CurrentSlot().palm = event.value == MtToolPalm;
                break;
            case AbsMtPositionX: {
                Slot& slot = CurrentSlot();
                slot.x = event.value;
                slot.moved = true;
                break;
            }
            case AbsMtPositionY: {
                Slot& slot = CurrentSlot();
                slot.y = event.value;
                slot.moved = true;
                break;
            }
            default:
                break;
        }
    } else if (event.kind == EvKey && event.code == BtnTouch) {
        pressed_ = event.value != 0;
    } else if (event.kind == EvSyn && event.code == SynReport) {
        Flush(out);
    }
}

bool SlotsParser::IsIdle() const {
    return std::ranges::none_of(slots_, [](const Slot& slot) { return slot.live.has_value(); });
}

void SlotsParser::Cancel(std::vector<Contact>& out) {
    for (Slot& slot : slots_) {
        if (const std::optional<Held> held = std::exchange(slot.live, std::nullopt)) {
            out.push_back(MakeContact(held->id, Phase::Cancel, {slot.x, slot.y, std::nullopt}));
        }
    }
    slots_.clear();
    pressed_ = false;
}

void SlotsParser::RestoreSlots(const AxisState& state, bool pressed, std::vector<Contact>& out) {
    const std::span<const int32_t> tracking = state.Column(AbsMtTrackingId);
    const std::span<const int32_t> x = state.Column(AbsMtPositionX);
    const std::span<const int32_t> y = state.Column(AbsMtPositionY);
    const std::span<const int32_t> pressure = state.Column(AbsMtPressure);
    const std::span<const int32_t> tool = state.Column(AbsMtToolType);
    slots_.clear();
    for (size_t slot = 0; slot < std::min(tracking.size(), MaxContacts); slot++) {
        Slot restored;
        restored.tracking = tracking[slot];
        restored.x = ValueAt(x, slot);
        restored.y = ValueAt(y, slot);
        restored.pressure = ValueAt(pressure, slot);
        restored.palm = ValueAt(tool, slot) == MtToolPalm;
        slots_.push_back(restored);
    }
    current_ = state.slot < 0 ? 0 : std::min(static_cast<size_t>(state.slot), MaxContacts - 1);
    pressed_ = pressed;
    Flush(out);
}

SlotsParser::Slot& SlotsParser::CurrentSlot() {
    if (slots_.size() <= current_) {
        slots_.resize(current_ + 1);
    }
    return slots_[current_];
}

void SlotsParser::Flush(std::vector<Contact>& out) {
    for (Slot& slot : slots_) {
        const Point point{slot.x, slot.y, std::nullopt};
        const bool wanted = slot.tracking >= 0 && !Hovers(pressed_, slot.pressure, slot.palm);
        if (slot.live && wanted && slot.live->tracking == slot.tracking) {
            if (slot.moved) {
                out.push_back(MakeContact(slot.live->id, Phase::Move, point));
            }
        } else {
            if (slot.live) {
                out.push_back(MakeContact(slot.live->id, Phase::End, point));
            }
            slot.live = wanted ? std::optional(Held{FreshId(), slot.tracking}) : std::nullopt;
            if (slot.live) {
                out.push_back(MakeContact(slot.live->id, Phase::Start, point));
            }
        }
        slot.moved = false;
    }
}

FramesParser::FramesParser(const Panel& setup) : TouchParser(setup) {
}

std::optional<nanoseconds> FramesParser::Deadline() const {
    return releaseAt_;
}

void FramesParser::Feed(RawEvent event, nanoseconds now, std::vector<Contact>& out) {
    if (event.kind == EvAbs) {
        switch (event.code) {
            case AbsMtPositionX:
                partial_.x = event.value;
                break;
            case AbsMtPositionY:
                partial_.y = event.value;
                break;
            case AbsMtTrackingId:
                partial_.tracking = event.value;
                break;
            case AbsMtPressure:
                partial_.pressure = event.value;
                break;
            default:
                break;
        }
    } else if (event.kind == EvKey && event.code == BtnTouch) {
        pressed_ = event.value != 0;
    } else if (event.kind == EvSyn && event.code == SynMtReport) {
        PushPartial();
    } else if (event.kind == EvSyn && event.code == SynReport) {
        PushPartial();
        Tick(now, out);
        Commit(now, out);
    }
}

void FramesParser::Tick(nanoseconds now, std::vector<Contact>& out) {
    if (releaseAt_ && now >= *releaseAt_) {
        Finish(Phase::End, out);
    }
}

bool FramesParser::IsIdle() const {
    return live_.empty();
}

void FramesParser::Cancel(std::vector<Contact>& out) {
    Finish(Phase::Cancel, out);
    frame_.clear();
    partial_ = {};
    pressed_ = false;
}

void FramesParser::Finish(Phase phase, std::vector<Contact>& out) {
    for (const Live& live : live_) {
        out.push_back(MakeContact(live.id, phase, live.point));
    }
    live_.clear();
    heldSince_.reset();
    releaseAt_.reset();
    fading_ = false;
}

void FramesParser::PushPartial() {
    const Partial partial = std::exchange(partial_, {});
    if (!partial.x && !partial.y) {
        return;
    }
    last_ = {partial.x.value_or(last_.x), partial.y.value_or(last_.y), partial.tracking};
    const bool hovering = setup_.pressure && partial.pressure && *partial.pressure <= 0;
    if (!hovering && frame_.size() < MaxContacts) {
        frame_.push_back(last_);
    }
}

void FramesParser::Commit(nanoseconds now, std::vector<Contact>& out) {
    using namespace std::chrono_literals;
    const bool tracked = setup_.protocol.tracked;
    const bool button = setup_.button;
    const bool unreliable = !tracked && !button;

    if (frame_.empty() || (button && !pressed_)) {
        if (!unreliable || live_.empty()) {
            Finish(Phase::End, out);
        } else if (!std::exchange(fading_, true)) {
            // A contact held for longer than a tap takes is given longer to come back.
            const bool longHold = heldSince_ && std::max(now - *heldSince_, nanoseconds::zero()) >= 200ms;
            releaseAt_ = now + (longHold ? nanoseconds(250ms) : nanoseconds(40ms));
        }
        frame_.clear();
        return;
    }

    const std::vector<Point>& points = frame_;
    const std::vector<Live> previous = std::exchange(live_, {});
    std::array<bool, MaxContacts> taken{};
    std::array<std::optional<size_t>, MaxContacts> owner{};
    if (tracked) {
        for (size_t index = 0; index < points.size(); index++) {
            const auto same = std::ranges::find_if(previous, [&](const Live& live) { return live.point.tracking == points[index].tracking; });
            if (same != previous.end()) {
                owner[index] = static_cast<size_t>(same - previous.begin());
            }
        }
    } else {
        for (size_t round = 0; round < std::min(previous.size(), points.size()); round++) {
            struct Pair {
                int64_t distance;
                size_t live;
                size_t point;
            };
            std::optional<Pair> best;
            for (size_t liveIndex = 0; liveIndex < previous.size(); liveIndex++) {
                if (taken[liveIndex]) {
                    continue;
                }
                for (size_t pointIndex = 0; pointIndex < points.size(); pointIndex++) {
                    if (owner[pointIndex]) {
                        continue;
                    }
                    // The difference wraps around where a plain one would overflow, which is undefined.
                    const auto apart = [](int32_t left, int32_t right) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(left) - static_cast<uint32_t>(right)))); };
                    const uint64_t dx = apart(points[pointIndex].x, previous[liveIndex].point.x);
                    const uint64_t dy = apart(points[pointIndex].y, previous[liveIndex].point.y);
                    const int64_t distance = static_cast<int64_t>(dx * dx + dy * dy);
                    if (!best || distance < best->distance) {
                        best = Pair{distance, liveIndex, pointIndex};
                    }
                }
            }
            if (!best) {
                break;
            }
            taken[best->live] = true;
            owner[best->point] = best->live;
        }
    }
    for (size_t index = 0; index < points.size(); index++) {
        if (owner[index]) {
            taken[*owner[index]] = true;
        }
    }

    for (size_t index = 0; index < previous.size(); index++) {
        if (!taken[index]) {
            out.push_back(MakeContact(previous[index].id, Phase::End, previous[index].point));
        }
    }
    for (size_t index = 0; index < points.size(); index++) {
        const Point& point = points[index];
        uint32_t id;
        if (owner[index]) {
            const Live& live = previous[*owner[index]];
            if (live.point.x != point.x || live.point.y != point.y) {
                out.push_back(MakeContact(live.id, Phase::Move, point));
            }
            id = live.id;
        } else {
            id = FreshId();
            out.push_back(MakeContact(id, Phase::Start, point));
        }
        live_.push_back({id, point});
    }

    fading_ = false;
    if (previous.empty() || !heldSince_) {
        heldSince_ = now;
    }
    // A panel that cannot say a finger is gone is taken to have none after a second of silence.
    releaseAt_ = unreliable ? std::optional(now + nanoseconds(1s)) : std::nullopt;
    frame_.clear();
}

SingleParser::SingleParser(const Panel& setup) : TouchParser(setup) {
}

void SingleParser::Feed(RawEvent event, nanoseconds, std::vector<Contact>& out) {
    if (event.kind == EvAbs) {
        switch (event.code) {
            case AbsX:
                x_ = event.value;
                break;
            case AbsY:
                y_ = event.value;
                break;
            case AbsPressure:
                pressure_ = event.value;
                break;
            default:
                break;
        }
    } else if (event.kind == EvKey && event.code == BtnTouch) {
        pressed_ = event.value != 0;
    } else if (event.kind == EvSyn && event.code == SynReport) {
        const std::optional<int32_t> x = std::exchange(x_, std::nullopt), y = std::exchange(y_, std::nullopt);
        const bool moved = x || y;
        last_ = {x.value_or(last_.x), y.value_or(last_.y), std::nullopt};
        const bool touchingNow = pressed_ && !(setup_.pressure && pressure_ <= 0);
        if (!live_ && touchingNow) {
            live_ = Live{FreshId(), last_};
            out.push_back(MakeContact(live_->id, Phase::Start, last_));
        } else if (live_ && touchingNow && moved) {
            live_->point = last_;
            out.push_back(MakeContact(live_->id, Phase::Move, last_));
        } else if (live_ && !touchingNow) {
            Finish(Phase::End, out);
        }
    }
}

bool SingleParser::IsIdle() const {
    return !live_;
}

void SingleParser::Cancel(std::vector<Contact>& out) {
    Finish(Phase::Cancel, out);
    x_.reset();
    y_.reset();
    pressed_ = false;
}

void SingleParser::Finish(Phase phase, std::vector<Contact>& out) {
    if (const std::optional<Live> live = std::exchange(live_, std::nullopt)) {
        out.push_back(MakeContact(live->id, phase, live->point));
    }
}

static constexpr float WheelDetent = 120.0f;

DeviceParser::DeviceParser(const Capabilities& device, const Roles& roles) : pointer_(roles.pointer), keyboard_(roles.keyboard), wheel_(roles.wheel), preciseWheel_(device.HasRelative(RelWheelHiRes) || device.HasRelative(RelHwheelHiRes)) {
    if (roles.touch) {
        touch_ = TouchParser::Create(*roles.touch);
    }
}

TouchParser* DeviceParser::TouchMut() {
    return touch_.get();
}

std::optional<nanoseconds> DeviceParser::Deadline() const {
    return touch_ ? touch_->Deadline() : std::nullopt;
}

bool DeviceParser::IsIdle() const {
    return !touch_ || touch_->IsIdle();
}

bool DeviceParser::IsTouching(const AxisState& state, bool pressed) const {
    return touch_ && touch_->IsTouching(state, pressed);
}

void DeviceParser::Feed(RawEvent event, nanoseconds now, std::vector<Sample>& out) {
    if (touch_) {
        touch_->Feed(event, now, contacts_);
        PassContacts(out);
    }

    const int32_t value = event.value;
    const uint16_t code = event.code;
    if (event.kind == EvAbs) {
        if (pointer_ && code == AbsX) {
            position_[0] = value;
            moved_ = true;
        } else if (pointer_ && code == AbsY) {
            position_[1] = value;
            moved_ = true;
        }
    } else if (event.kind == EvKey) {
        if (pointer_ && (code == BtnLeft || code == BtnTouch)) {
            buttons_.push_back({MouseButton::Left, value != 0});
        } else if (pointer_ && code == BtnRight) {
            buttons_.push_back({MouseButton::Right, value != 0});
        } else if (pointer_ && code == BtnMiddle) {
            buttons_.push_back({MouseButton::Middle, value != 0});
        } else if (keyboard_ && !(code >= BtnMisc && code < 0x160)) {
            // From `BTN_MISC` up to `KEY_OK` (0x160) the codes are buttons of mice, pads and pens.
            out.push_back({Sample::Key{code, value != 0, value == 2}});
        }
    } else if (event.kind == EvRel) {
        if (wheel_ && !preciseWheel_ && code == RelWheel) {
            scroll_[1] += static_cast<float>(value);
        } else if (wheel_ && !preciseWheel_ && code == RelHwheel) {
            scroll_[0] += static_cast<float>(value);
        } else if (wheel_ && code == RelWheelHiRes) {
            scroll_[1] += static_cast<float>(value) / WheelDetent;
        } else if (wheel_ && code == RelHwheelHiRes) {
            scroll_[0] += static_cast<float>(value) / WheelDetent;
        }
    } else if (event.kind == EvSyn && code == SynReport) {
        Flush(out);
    }
}

void DeviceParser::Tick(nanoseconds now, std::vector<Sample>& out) {
    if (touch_) {
        touch_->Tick(now, contacts_);
        PassContacts(out);
    }
}

void DeviceParser::Cancel(std::vector<Sample>& out) {
    if (touch_) {
        touch_->Cancel(contacts_);
        PassContacts(out);
    }
    buttons_.clear();
    scroll_ = {};
}

void DeviceParser::PassContacts(std::vector<Sample>& out) {
    for (const Contact& contact : contacts_) {
        out.push_back({contact});
    }
    contacts_.clear();
}

void DeviceParser::Flush(std::vector<Sample>& out) {
    if (pointer_ && std::exchange(moved_, false)) {
        out.push_back({Sample::Hover{pointer_->x.Normalize(position_[0]), pointer_->y.Normalize(position_[1])}});
    }
    for (const Sample::Button& button : buttons_) {
        out.push_back({button});
    }
    buttons_.clear();
    const auto [x, y] = std::exchange(scroll_, {});
    if (x != 0.0f || y != 0.0f) {
        out.push_back({Sample::Wheel{x, y}});
    }
}
