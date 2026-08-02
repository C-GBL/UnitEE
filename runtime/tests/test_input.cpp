// M10 task 2: pad mapping, edge detection and the analog deadzone. All of
// it is testable without hardware through inject_frame, which is also how a
// recorded-input replay would drive the game.
#include "ps2ur/input.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace ps2ur;
using namespace ps2ur::input;

namespace {

// libpad reports buttons ACTIVE LOW. These helpers keep the tests written
// in the driver's language so the inversion is genuinely exercised.
constexpr uint16_t kNoButtons = 0xFFFF;

uint16_t held(std::initializer_list<Button> buttons)
{
    uint16_t word = kNoButtons;
    for (Button b : buttons) {
        word &= static_cast<uint16_t>(~(1u << static_cast<uint32_t>(b)));
    }
    return word;
}

struct InputFixture {
    InputFixture()
    {
        shutdown();
        init();
        set_deadzone(0.2f);
    }
    ~InputFixture() { shutdown(); }
};

} // namespace

TEST(Input, ActiveLowButtonsAreInvertedExactlyOnce)
{
    InputFixture fixture;
    // Nothing held: every query false, even though the raw word is all ones.
    inject_frame(0, kNoButtons, 128, 128, 128, 128, nullptr);
    EXPECT_FALSE(button(0, Button::Cross));
    EXPECT_FALSE(button(0, Button::Start));

    inject_frame(0, held({Button::Cross, Button::R1}), 128, 128, 128, 128,
                 nullptr);
    EXPECT_TRUE(button(0, Button::Cross));
    EXPECT_TRUE(button(0, Button::R1));
    EXPECT_FALSE(button(0, Button::Circle));
}

TEST(Input, EdgesFireOnceEach)
{
    InputFixture fixture;
    inject_frame(0, kNoButtons, 128, 128, 128, 128, nullptr);
    EXPECT_FALSE(button_down(0, Button::Cross));

    inject_frame(0, held({Button::Cross}), 128, 128, 128, 128, nullptr);
    EXPECT_TRUE(button_down(0, Button::Cross));
    EXPECT_TRUE(button(0, Button::Cross));
    EXPECT_FALSE(button_up(0, Button::Cross));

    // Still held next frame: down must NOT fire again.
    inject_frame(0, held({Button::Cross}), 128, 128, 128, 128, nullptr);
    EXPECT_FALSE(button_down(0, Button::Cross));
    EXPECT_TRUE(button(0, Button::Cross));

    inject_frame(0, kNoButtons, 128, 128, 128, 128, nullptr);
    EXPECT_TRUE(button_up(0, Button::Cross));
    EXPECT_FALSE(button(0, Button::Cross));

    inject_frame(0, kNoButtons, 128, 128, 128, 128, nullptr);
    EXPECT_FALSE(button_up(0, Button::Cross));
}

TEST(Input, PortsAreIndependent)
{
    InputFixture fixture;
    inject_frame(0, held({Button::Cross}), 128, 128, 128, 128, nullptr);
    inject_frame(1, held({Button::Triangle}), 128, 128, 128, 128, nullptr);
    EXPECT_TRUE(button(0, Button::Cross));
    EXPECT_FALSE(button(1, Button::Cross));
    EXPECT_TRUE(button(1, Button::Triangle));
    EXPECT_FALSE(button(0, Button::Triangle));

    // Out-of-range ports are silent, not undefined.
    EXPECT_FALSE(button(9, Button::Cross));
    EXPECT_FALSE(connected(9));
    EXPECT_EQ(raw_axis(9, false, false), 128);
}

TEST(Input, DeadzoneSuppressesCentreDriftAndStillReachesFullScale)
{
    // A centred stick reads exactly zero on BOTH halves; the byte range is
    // asymmetric (128 below centre, 127 above), so a naive /128 leaves a
    // resting stick slightly positive.
    EXPECT_FLOAT_EQ(apply_deadzone(128, 0.2f), 0.0f);
    EXPECT_FLOAT_EQ(apply_deadzone(127, 0.0f), -1.0f / 128.0f);

    // Inside the deadzone: silent.
    EXPECT_FLOAT_EQ(apply_deadzone(140, 0.2f), 0.0f);
    EXPECT_FLOAT_EQ(apply_deadzone(116, 0.2f), 0.0f);

    // Extremes still reach full scale rather than stopping short.
    EXPECT_NEAR(apply_deadzone(255, 0.2f), 1.0f, 1e-5f);
    EXPECT_NEAR(apply_deadzone(0, 0.2f), -1.0f, 1e-5f);

    // And the curve is continuous at the deadzone edge: just outside it the
    // value is near zero, not a jump to 0.2.
    const float justOutside = apply_deadzone(static_cast<uint8_t>(128 + 27),
                                             0.2f);
    EXPECT_GT(justOutside, 0.0f);
    EXPECT_LT(justOutside, 0.05f);
}

TEST(Input, ZeroDeadzonePassesTheRawCurveThrough)
{
    EXPECT_NEAR(apply_deadzone(255, 0.0f), 1.0f, 1e-5f);
    EXPECT_NEAR(apply_deadzone(0, 0.0f), -1.0f, 1e-5f);
    EXPECT_NEAR(apply_deadzone(192, 0.0f), 0.5039f, 1e-3f);
}

TEST(Input, VerticalAxisIsPositiveUp)
{
    InputFixture fixture;
    // The pad's vertical byte grows DOWNWARD; Unity's Y axis is up, so
    // pushing the stick up (byte 0) must read +1.
    inject_frame(0, kNoButtons, 128, 0, 128, 255, nullptr);
    EXPECT_NEAR(axis_y(0, false), 1.0f, 1e-5f);
    EXPECT_NEAR(axis_y(0, true), -1.0f, 1e-5f);
    EXPECT_FLOAT_EQ(axis_x(0, false), 0.0f);
}

TEST(Input, HorizontalAxisSignMatchesTheStick)
{
    InputFixture fixture;
    inject_frame(0, kNoButtons, 255, 128, 0, 128, nullptr);
    EXPECT_NEAR(axis_x(0, false), 1.0f, 1e-5f);  // left stick right
    EXPECT_NEAR(axis_x(0, true), -1.0f, 1e-5f);  // right stick left
}

TEST(Input, DeadzoneIsConfigurable)
{
    InputFixture fixture;
    inject_frame(0, kNoButtons, 160, 128, 128, 128, nullptr);
    set_deadzone(0.5f);
    EXPECT_FLOAT_EQ(axis_x(0, false), 0.0f); // 0.25 of full scale, suppressed
    set_deadzone(0.05f);
    EXPECT_GT(axis_x(0, false), 0.1f);
    EXPECT_FLOAT_EQ(deadzone(), 0.05f);
}

TEST(Input, PressureIsReportedPerButton)
{
    InputFixture fixture;
    uint8_t pressures[16] = {};
    pressures[static_cast<uint32_t>(Button::Cross)] = 200;
    pressures[static_cast<uint32_t>(Button::Circle)] = 30;
    inject_frame(0, held({Button::Cross, Button::Circle}), 128, 128, 128, 128,
                 pressures);

    EXPECT_TRUE(pressure_available(0));
    EXPECT_EQ(pressure(0, Button::Cross), 200);
    EXPECT_EQ(pressure(0, Button::Circle), 30);
    EXPECT_EQ(pressure(0, Button::Square), 0);
}
