#include "Window/Mirror.h"

#include "Test.h"
#include "Window/ANativeWindowCreator.h"

#include <cmath>

class MirrorTest {
protected:
    static constexpr const char* Dump = R"(
Display Devices: size=3
  DisplayDeviceInfo{"Built-in Screen": uniqueId="local:4630946218744042899", 1504 x 2400, modeId 1, supportedModes [{id=1, width=1504, height=2400, fps=60.0}], state ON}
    mAdapter=LocalDisplayAdapter
    mUniqueId=local:4630946218744042899
    mCurrentLayerStack=0
    mCurrentFlags=1
    mCurrentOrientation=1
    mCurrentLayerStackRect=Rect(0, 0 - 2400, 1504)
    mCurrentDisplayRect=Rect(0, 0 - 2400, 1504)
  DisplayDeviceInfo{"ScreenRecorder": uniqueId="virtual:com.android.systemui,10182,ScreenRecorder,0", 1280 x 720, modeId 2, state ON}
    mUniqueId=virtual:com.android.systemui,10182,ScreenRecorder,0
    mCurrentLayerStack=7
    mCurrentOrientation=0
    mCurrentLayerStackRect=Rect(0, 0 - 1280, 720)
    mCurrentDisplayRect=Rect(0, 0 - 1280, 720)
  DisplayDeviceInfo{"Sleeping": uniqueId="local:2", 800 x 600, state OFF}
    mUniqueId=local:2
    mCurrentLayerStack=-1
    mCurrentLayerStackRect=Rect(0, 0 - 800, 600)
Logical Displays: size=1
)";
};

TEST(MirrorTest, "displays are read from the dump") {
    const std::vector<ShownDisplay> shown = Mirror::ParseDisplays(Dump);
    EXPECT_EQ(shown.size(), size_t{2});
    if (shown.size() == 2) {
        EXPECT(shown[0] == (ShownDisplay{0, {2400.0f, 1504.0f}}));
        EXPECT(shown[1] == (ShownDisplay{7, {1280.0f, 720.0f}}));
    }
    EXPECT(Mirror::ParseDisplays("nothing of the kind").empty());
}

TEST(MirrorTest, "a picture is scaled to fit and centred") {
    EXPECT(Mirror::Fit({2400.0f, 1504.0f}, {1200.0f, 752.0f}) == (MirrorPlacement{0.5f, {0.0f, 0.0f}}));
    MirrorPlacement placed = Mirror::Fit({2400.0f, 1504.0f}, {1280.0f, 720.0f});
    EXPECT_NEAR(placed.scale, 720.0 / 1504.0, 1e-6);
    EXPECT_NEAR(placed.offset[0], (1280.0 - 2400.0 * placed.scale) / 2.0, 1e-3);
    EXPECT_NEAR(placed.offset[1], 0.0, 1e-3);
    placed = Mirror::Fit({1504.0f, 2400.0f}, {1280.0f, 720.0f});
    EXPECT_NEAR(placed.scale, 0.3, 1e-6);
    EXPECT(placed.offset[0] > 400.0f);
    EXPECT_NEAR(placed.offset[1], 0.0, 1e-3);
}

TEST(MirrorTest, "regions intersect and unite") {
    const Region a{0, 0, 10, 10}, b{5, 5, 20, 8}, nothing{};
    EXPECT(a.Intersect(b) == (Region{5, 5, 10, 8}));
    EXPECT(a.Unite(b) == (Region{0, 0, 20, 10}));
    EXPECT(a.Unite(nothing) == a);
    EXPECT(nothing.Unite(b) == b);
    EXPECT(a.Intersect(Region{10, 0, 20, 10}).IsEmpty());
    EXPECT_EQ(a.GetArea(), int64_t{100});
    EXPECT_EQ(nothing.GetArea(), int64_t{0});
}

TEST(MirrorTest, "a display knows its rotation in degrees") {
    EXPECT_EQ((DisplayInfo{.rotation = Rotation::Deg0}).GetDegrees(), uint32_t{0});
    EXPECT_EQ((DisplayInfo{.rotation = Rotation::Deg180}).GetDegrees(), uint32_t{180});
    EXPECT_EQ((DisplayInfo{.rotation = Rotation::Deg270}).GetDegrees(), uint32_t{270});
}
