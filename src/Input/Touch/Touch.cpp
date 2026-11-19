#include "Input/Touch/Touch.h"

#include "Utils/Logger.h"

#include <algorithm>
#include <atomic>
#include <thread>

// Fingers of gestures are numbered apart from those the application names itself.
static constexpr uint32_t GestureFingers = 1u << 16;

Touch::Touch() = default;

Touch::Touch(std::shared_ptr<TouchInjector> injector) : injector_(std::move(injector)) {
}

// The readers go with the last of those who hold them: here, or with the graphics.
Touch::~Touch() = default;

bool Touch::Fail(const Error& error) {
    logger.Error("{}", error.message);
    const std::scoped_lock lock(mutex_);
    error_ = error.message;
    return false;
}

std::string Touch::GetError() const {
    const std::scoped_lock lock(mutex_);
    return error_;
}

// A reference of the caller's own: `close` on another thread does not take what it drives away.
std::shared_ptr<TouchInjector> Touch::GetInjector() const {
    const std::scoped_lock lock(mutex_);
    return injector_;
}

bool Touch::SetControl(bool enabled) {
    const std::shared_ptr<TouchInjector> injector = GetInjector();
    if (!injector) {
        return Fail(Error{"touch is not started"});
    }
    const Result<> set = injector->SetControl(enabled);
    return set ? true : Fail(set.error());
}

bool Touch::IsControl() const {
    const std::shared_ptr<TouchInjector> injector = GetInjector();
    return injector && injector->IsControl();
}

bool Touch::Play(uint32_t finger, Phase phase, std::array<float, 2> position) {
    const std::shared_ptr<TouchInjector> injector = GetInjector();
    if (!injector) {
        return Fail(Error{"touch is not started"});
    }
    const Result<> played = injector->MoveFinger(finger, phase, position);
    return played ? true : Fail(played.error());
}

bool Touch::Down(uint32_t finger, std::array<float, 2> position) {
    return Play(finger, Phase::Start, position);
}

bool Touch::Move(uint32_t finger, std::array<float, 2> position) {
    return Play(finger, Phase::Move, position);
}

bool Touch::Up(uint32_t finger) {
    return Play(finger, Phase::End, {0.0f, 0.0f});
}

bool Touch::Tap(std::array<float, 2> position) {
    // A finger that stays down for 60 milliseconds is a tap to everything that watches.
    return Swipe(position, position, std::chrono::milliseconds(60));
}

bool Touch::Swipe(std::array<float, 2> from, std::array<float, 2> to, std::chrono::milliseconds duration) {
    using Clock = std::chrono::steady_clock;
    static std::atomic<uint32_t> next{GestureFingers};
    const uint32_t finger = next.fetch_add(1, std::memory_order_relaxed) | GestureFingers;
    const Clock::time_point started = Clock::now();
    if (!Down(finger, from)) {
        return false;
    }
    bool dragged = true;
    for (;;) {
        const auto left = std::chrono::duration_cast<Clock::duration>(duration) - (Clock::now() - started);
        // A step every 8 milliseconds: about what a touch panel reports.
        std::this_thread::sleep_for(std::clamp<Clock::duration>(left, Clock::duration::zero(), std::chrono::milliseconds(8)));
        const float progress = duration.count() <= 0 ? 1.0f : std::min(std::chrono::duration<float>(Clock::now() - started) / std::chrono::duration<float>(duration), 1.0f);
        dragged = Move(finger, {from[0] + (to[0] - from[0]) * progress, from[1] + (to[1] - from[1]) * progress});
        if (!dragged || progress >= 1.0f) {
            break;
        }
    }
    // The finger is lifted even when the drag went wrong.
    return Up(finger) && dragged;
}
