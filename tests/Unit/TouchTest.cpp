#include "Input/Touch/Touch.h"

#include "Test.h"

#include <mutex>
#include <vector>

class TouchTest {
protected:
    struct Step {
        uint32_t finger;
        Phase phase;
        std::array<float, 2> position;
    };

    class Recorder final : public TouchInjector {
    public:
        Result<> SetControl(bool) override {
            return {};
        }
        bool IsControl() const override {
            return true;
        }
        Result<> MoveFinger(uint32_t finger, Phase phase, std::array<float, 2> position) override {
            const std::scoped_lock lock(mutex_);
            steps_.push_back({finger, phase, position});
            return {};
        }

        std::vector<Step> Steps() {
            const std::scoped_lock lock(mutex_);
            return steps_;
        }

    private:
        std::mutex mutex_;
        std::vector<Step> steps_;
    };
};

TEST(TouchTest, "a swipe runs from end to end with one finger") {
    const auto recorder = std::make_shared<Recorder>();
    EXPECT(Touch(recorder).Swipe({100.0f, 200.0f}, {300.0f, 600.0f}, std::chrono::milliseconds(40)));
    const std::vector<Step> steps = recorder->Steps();
    EXPECT(steps.size() >= 4);
    if (steps.size() < 4) {
        return;
    }
    const Step& first = steps.front();
    const Step& last = steps.back();
    const Step& before = steps[steps.size() - 2];
    EXPECT(first.phase == Phase::Start && first.position == (std::array{100.0f, 200.0f}));
    EXPECT(last.phase == Phase::End);
    EXPECT(before.phase == Phase::Move && before.position == (std::array{300.0f, 600.0f}));
    for (const Step& step : steps) {
        EXPECT_EQ(step.finger, first.finger);
    }
    for (size_t index = 1; index + 2 < steps.size(); index++) {
        EXPECT(steps[index].position[0] <= steps[index + 1].position[0]);
    }
}

TEST(TouchTest, "a tap goes down and up in one place") {
    const auto recorder = std::make_shared<Recorder>();
    EXPECT(Touch(recorder).Tap({50.0f, 60.0f}));
    const std::vector<Step> steps = recorder->Steps();
    EXPECT(steps.size() >= 3);
    if (steps.empty()) {
        return;
    }
    EXPECT(steps.front().phase == Phase::Start && steps.back().phase == Phase::End);
    for (size_t index = 0; index + 1 < steps.size(); index++) {
        EXPECT(steps[index].position == (std::array{50.0f, 60.0f}));
    }
}
