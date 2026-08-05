// Profiler tests (plan section 9, M13 task 1).
//
// What is worth asserting about a profiler is not "does it produce a number"
// but "is the number the right one when the shape of the frame is known".
// These build known shapes -- nested zones, a zone that runs twice, a frame
// with no zones at all -- and check the arithmetic that a human reading the
// overlay is about to trust.
#include "ps2ur/profiler.h"

#include "ps2ur/meminfo.h"
#include "ps2ur/platform.h"

#include <cstdio>
#include <cstring>

#include <gtest/gtest.h>

using namespace ps2ur;

namespace {

// Burns real ticks. Sleeping would be the obvious thing, but the point is to
// measure the same counter the profiler reads, so this spins on it.
void burn_ticks(uint64_t ticks)
{
    const uint64_t start = platform::now_ticks();
    volatile uint32_t sink = 0;
    while (platform::now_ticks() - start < ticks) {
        sink = sink + 1u;
    }
    (void)sink;
}

uint64_t ms_in_ticks(float ms)
{
    return static_cast<uint64_t>(
        (static_cast<float>(platform::ticks_per_second()) * ms) / 1000.0f);
}

class ProfilerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        platform::init();
        mem::reset();
        prof::reset();
        prof::set_enabled(true);
    }
    void TearDown() override { prof::reset(); }
};

} // namespace

TEST_F(ProfilerTest, RegistersZonesOnceAndReturnsAStableId)
{
    const uint32_t a = prof::zone_id("update");
    const uint32_t b = prof::zone_id("render");
    const uint32_t again = prof::zone_id("update");

    EXPECT_EQ(a, again);
    EXPECT_NE(a, b);
    EXPECT_EQ(2u, prof::zone_count());
}

TEST_F(ProfilerTest, DedupesZonesByNameContentNotPointer)
{
    // The same literal in two translation units is not guaranteed to be the
    // same address. A profiler that deduped by pointer would silently split
    // one zone in two and halve both.
    char name_a[] = "physics";
    char name_b[] = "physics";
    ASSERT_NE(static_cast<const void*>(name_a), static_cast<const void*>(name_b));

    EXPECT_EQ(prof::zone_id(name_a), prof::zone_id(name_b));
    EXPECT_EQ(1u, prof::zone_count());
}

TEST_F(ProfilerTest, MeasuresAZoneAgainstTheClockItReports)
{
    const uint32_t id = prof::zone_id("work");
    const uint64_t target = ms_in_ticks(4.0f);

    prof::begin_frame();
    prof::zone_begin(id);
    burn_ticks(target);
    prof::zone_end(id);
    prof::end_frame();

    const float ms = prof::ticks_to_ms(prof::zone(id).max_inclusive);
    // Generous bounds: this runs on a loaded developer machine, and the
    // assertion that matters is that the number is the measured duration
    // rather than zero or a wild misconversion.
    EXPECT_GT(ms, 3.0f);
    EXPECT_LT(ms, 40.0f);
}

TEST_F(ProfilerTest, SeparatesInclusiveFromExclusiveWhenZonesNest)
{
    const uint32_t outer = prof::zone_id("outer");
    const uint32_t inner = prof::zone_id("inner");
    const uint64_t chunk = ms_in_ticks(3.0f);

    prof::begin_frame();
    prof::zone_begin(outer);
    burn_ticks(chunk);          // outer only
    prof::zone_begin(inner);
    burn_ticks(chunk);          // both
    prof::zone_end(inner);
    prof::zone_end(outer);
    prof::end_frame();

    const ps2ur::prof::ZoneStats& o = prof::zone(outer);
    const ps2ur::prof::ZoneStats& i = prof::zone(inner);

    // Inclusive outer covers both chunks; exclusive covers only its own.
    EXPECT_GT(o.max_inclusive, i.max_inclusive);
    EXPECT_GT(o.inclusive, 0u);
    // Exclusive is what finds the culprit, so it must not include the child.
    EXPECT_LT(o.exclusive, o.inclusive);
    // The child's time is the difference, within measurement noise.
    const float diff_ms = prof::ticks_to_ms(o.inclusive - o.exclusive);
    const float inner_ms = prof::ticks_to_ms(i.inclusive);
    EXPECT_NEAR(diff_ms, inner_ms, 2.0f);
}

TEST_F(ProfilerTest, CountsRepeatedCallsWithinOneFrame)
{
    const uint32_t id = prof::zone_id("repeated");

    prof::begin_frame();
    for (int i = 0; i < 5; ++i) {
        prof::zone_begin(id);
        burn_ticks(ms_in_ticks(0.5f));
        prof::zone_end(id);
    }
    prof::end_frame();

    // calls is reset per frame, so read it through the history instead: the
    // frame's inclusive total should be about five slices.
    EXPECT_GE(prof::history_count(), 1u);
    const float total = prof::ticks_to_ms(prof::history_zone(0, id));
    EXPECT_GT(total, 1.5f);
}

TEST_F(ProfilerTest, ScopeMacroMeasuresTheEnclosingScope)
{
    prof::begin_frame();
    {
        PS2UR_PROFILE_ZONE("scoped");
        burn_ticks(ms_in_ticks(2.0f));
    }
    prof::end_frame();

    const uint32_t id = prof::zone_id("scoped");
    EXPECT_GT(prof::zone(id).max_inclusive, 0u);
}

TEST_F(ProfilerTest, KeepsAFrameHistoryRingThatSaturates)
{
    const uint32_t id = prof::zone_id("tick");
    const uint32_t frames = prof::kHistoryFrames + 25;
    for (uint32_t i = 0; i < frames; ++i) {
        prof::begin_frame();
        prof::zone_begin(id);
        prof::zone_end(id);
        prof::end_frame();
    }

    EXPECT_EQ(prof::kHistoryFrames, prof::history_count());
    EXPECT_EQ(static_cast<uint64_t>(frames), prof::frames_elapsed());
}

TEST_F(ProfilerTest, BucketsGcPausesByDuration)
{
    prof::begin_frame();
    prof::record_gc_pause(static_cast<uint32_t>(ms_in_ticks(0.5f)));  // <1
    prof::record_gc_pause(static_cast<uint32_t>(ms_in_ticks(3.0f)));  // <4
    prof::record_gc_pause(static_cast<uint32_t>(ms_in_ticks(40.0f))); // <66
    prof::end_frame();

    const uint32_t* h = prof::gc_histogram();
    uint32_t total = 0;
    for (uint32_t i = 0; i < prof::kGcBuckets; ++i) {
        total += h[i];
    }
    EXPECT_EQ(3u, total);
    EXPECT_EQ(1u, h[0]); // the sub-millisecond pause
    EXPECT_EQ(3u, prof::last_counters().gc_pauses);

    // A pause past the frame budget must not land in a bucket that reads as
    // harmless: 40 ms belongs above the 33 ms edge.
    EXPECT_EQ(0u, h[5]);
    EXPECT_EQ(1u, h[6]);
}

TEST_F(ProfilerTest, DisablingStopsMeasurementWithoutLosingRegistration)
{
    const uint32_t id = prof::zone_id("gated");

    prof::set_enabled(false);
    prof::begin_frame();
    prof::zone_begin(id);
    burn_ticks(ms_in_ticks(2.0f));
    prof::zone_end(id);
    prof::end_frame();

    EXPECT_EQ(0u, prof::zone(id).max_inclusive);
    EXPECT_EQ(0u, prof::history_count());

    prof::set_enabled(true);
    prof::begin_frame();
    prof::zone_begin(id);
    burn_ticks(ms_in_ticks(2.0f));
    prof::zone_end(id);
    prof::end_frame();

    EXPECT_GT(prof::zone(id).max_inclusive, 0u);
}

TEST_F(ProfilerTest, SurvivesAMismatchedZoneEnd)
{
    // A stray end must not corrupt the stack: if it did, every enclosing
    // zone's time would be silently reassigned and the profile would lie
    // rather than fail.
    const uint32_t a = prof::zone_id("a");
    const uint32_t b = prof::zone_id("b");

    prof::begin_frame();
    prof::zone_begin(a);
    prof::zone_end(b); // never begun
    burn_ticks(ms_in_ticks(2.0f));
    prof::zone_end(a);
    prof::end_frame();

    EXPECT_GT(prof::zone(a).max_inclusive, 0u);
    EXPECT_EQ(0u, prof::zone(b).max_inclusive);
}

TEST_F(ProfilerTest, DumpsACsvWithOneRowPerRetainedFrameAndAColumnPerZone)
{
    const uint32_t a = prof::zone_id("update");
    const uint32_t b = prof::zone_id("render");
    const uint32_t frames = 6;
    for (uint32_t i = 0; i < frames; ++i) {
        prof::begin_frame();
        prof::zone_begin(a);
        burn_ticks(ms_in_ticks(1.0f));
        prof::zone_end(a);
        prof::zone_begin(b);
        burn_ticks(ms_in_ticks(1.0f));
        prof::zone_end(b);
        prof::record_gc_pause(static_cast<uint32_t>(ms_in_ticks(1.5f)));
        prof::end_frame();
    }

    const char* path = "ps2ur_profile_test.csv";
    ASSERT_TRUE(prof::dump_csv(path));

    FILE* f = std::fopen(path, "r");
    ASSERT_NE(nullptr, f);
    char header[1024] = {};
    ASSERT_NE(nullptr, std::fgets(header, sizeof(header), f));
    // The viewer keys off these names, so a rename here has to be a
    // deliberate change to both sides rather than a silent break.
    EXPECT_NE(nullptr, std::strstr(header, "frame_ms"));
    EXPECT_NE(nullptr, std::strstr(header, "dma_wait_ms"));
    EXPECT_NE(nullptr, std::strstr(header, "gc_pause_ms"));
    EXPECT_NE(nullptr, std::strstr(header, "update_ms"));
    EXPECT_NE(nullptr, std::strstr(header, "render_ms"));

    uint32_t rows = 0;
    char line[1024];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (line[0] != '\n' && line[0] != '\0') {
            rows++;
        }
    }
    std::fclose(f);
    std::remove(path);
    EXPECT_EQ(frames, rows);
}

TEST_F(ProfilerTest, ToleratesMoreZonesThanTheTableHolds)
{
    char names[prof::kMaxZones + 4][8];
    for (uint32_t i = 0; i < prof::kMaxZones + 4; ++i) {
        std::snprintf(names[i], sizeof(names[i]), "z%u", static_cast<unsigned>(i));
        const uint32_t id = prof::zone_id(names[i]);
        // Overflow returns kMaxZones, and every entry point treats that as a
        // no-op rather than writing past the table.
        prof::zone_begin(id);
        prof::zone_end(id);
    }
    EXPECT_EQ(prof::kMaxZones, prof::zone_count());
}
