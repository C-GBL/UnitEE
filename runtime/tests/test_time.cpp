// Frame timer unit tests (plan section 9, M1 task 5: "a frame timer").
//
// These assert structure and monotonicity, not wall-clock durations: the host
// build has no real-time guarantees and a test that sleeps is a test that goes
// flaky on a loaded CI machine.
#include "ps2ur/time.h"

#include "ps2ur/platform.h"

#include <gtest/gtest.h>

using namespace ps2ur;

TEST(Time, TicksPerSecondIsPositive)
{
    EXPECT_GT(platform::ticks_per_second(), 0u);
}

TEST(Time, TicksAreMonotonic)
{
    time::init();
    const uint64_t a = time::ticks();
    // Burn some cycles rather than sleeping; the clock must never go backwards.
    volatile uint64_t spin = 0;
    for (int i = 0; i < 200000; ++i) {
        spin += static_cast<uint64_t>(i);
    }
    const uint64_t b = time::ticks();
    EXPECT_GE(b, a);
}

TEST(Time, SecondsIsNonNegativeAndFinite)
{
    time::init();
    const float s = time::seconds();
    EXPECT_GE(s, 0.0f);
    // Guard against an uninitialised/garbage epoch producing something absurd.
    EXPECT_LT(s, 3600.0f);
}

TEST(Time, UpdateProducesNonNegativeDelta)
{
    time::init();
    time::update();
    time::update();
    const float dt = time::delta_seconds();
    EXPECT_GE(dt, 0.0f);
    EXPECT_LT(dt, 3600.0f);
}

TEST(Time, DeltaIsFloatNotDouble)
{
    // Plan section 3.1: doubles are soft-float on the EE and must never appear
    // in runtime code. This pins the signature so a well-meaning change to
    // `double` fails the build here rather than silently costing 20-100x.
    static_assert(sizeof(decltype(time::delta_seconds())) == sizeof(float),
                  "delta_seconds must be float; double is soft-float on the EE");
    static_assert(sizeof(decltype(time::seconds())) == sizeof(float),
                  "seconds must be float; double is soft-float on the EE");
    SUCCEED();
}
