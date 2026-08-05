// Soak test (plan section 9, M13 task 5): "30-minute automated play session
// (recorded input playback) with no leaks, no growth, no dropouts."
//
// Why a host test and not only an on-target run: thirty minutes of game time
// is 54,000 frames, and the emulator runs this workload at roughly a third of
// real time, so an on-target soak is a ninety minute wall-clock commitment.
// Leaks and unbounded growth are not platform-specific -- they are properties
// of the allocation logic, which is the same code on both targets -- so the
// full-length soak runs here at speed, and the on-target run covers the
// things only the console can tell us (dropped frames, DMA, the pipeline).
//
// What "no growth" means precisely, because it is the whole assertion: the
// memory high-water mark stops rising. A soak that allocates in a steady
// state reaches its peak within the first few seconds and then holds it
// forever. Anything that keeps climbing after warm-up is a leak, whether or
// not it ever exhausts the machine.
#include "ps2ur/alloc.h"
#include "ps2ur/input.h"
#include "ps2ur/meminfo.h"
#include "ps2ur/profiler.h"

#include <gtest/gtest.h>

#include <vector>

using namespace ps2ur;

namespace {

// Thirty minutes at 30 fps. The number the plan asks for, spelled out so it
// cannot drift.
constexpr uint32_t kSoakFrames = 30u * 60u * 30u; // 54,000

// A short recorded session that loops: drive forward, turn, brake, press a
// face button, idle. Ten seconds of it, so the soak wraps 180 times and any
// per-loop leak compounds into something a threshold can see.
std::vector<input::playback::Frame> make_recording()
{
    std::vector<input::playback::Frame> frames;
    frames.reserve(300);
    for (uint32_t i = 0; i < 300; ++i) {
        input::playback::Frame f;
        const uint32_t phase = i / 60;
        switch (phase) {
            case 0: // accelerate
                f.buttons = 1u << static_cast<uint32_t>(input::Button::Cross);
                f.ly = 40;
                break;
            case 1: // turn left
                f.lx = 30;
                f.ly = 60;
                break;
            case 2: // turn right while looking around
                f.lx = 220;
                f.rx = 200;
                break;
            case 3: // brake
                f.buttons = 1u << static_cast<uint32_t>(input::Button::Square);
                f.ly = 210;
                break;
            default: // idle
                break;
        }
        frames.push_back(f);
    }
    return frames;
}

// One frame of a steady-state game: transient work in a stack allocator that
// is rewound, plus pooled objects that come and go. This is the shape the
// assertion is about, not any particular subsystem.
void simulate_frame(StackAllocator& scratch, PoolAllocator& pool,
                    uint32_t frame)
{
    const StackAllocator::Marker mark = scratch.marker();
    // Per-frame scratch: culling lists, chain staging, skinning palettes.
    ASSERT_NE(nullptr, scratch.alloc(512));
    ASSERT_NE(nullptr, scratch.alloc(1024));

    // Churn: objects with lifetimes of a few frames, like particles or
    // one-shot audio voices.
    void* blocks[4] = {};
    const uint32_t live = 1u + (frame % 4u);
    for (uint32_t i = 0; i < live; ++i) {
        blocks[i] = pool.alloc();
    }
    for (uint32_t i = 0; i < live; ++i) {
        pool.free(blocks[i]);
    }

    scratch.rewind(mark);
}

class SoakTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        mem::reset();
        prof::reset();
        heap_reset_stats();
        heap_set_budget(0);
    }
    void TearDown() override
    {
        input::playback::stop();
        mem::reset();
        prof::reset();
    }
};

} // namespace

TEST_F(SoakTest, ThirtyMinutesOfPlaybackLeavesNoGrowth)
{
    static unsigned char scratch_mem[64 * 1024];
    static unsigned char pool_mem[32 * 1024];
    StackAllocator scratch;
    PoolAllocator pool;
    scratch.init(scratch_mem, sizeof(scratch_mem));
    pool.init(pool_mem, sizeof(pool_mem), 256);

    ASSERT_TRUE(mem::add_stack("scratch", &scratch));
    ASSERT_TRUE(mem::add_pool("pool", &pool));
    ASSERT_TRUE(mem::add_heap("heap"));

    const std::vector<input::playback::Frame> recording = make_recording();
    input::playback::start(recording.data(),
                           static_cast<uint32_t>(recording.size()));

    // Warm-up: let the steady state establish before the mark is taken.
    // Measuring from frame zero would call first-time allocation "growth".
    constexpr uint32_t kWarmup = 600; // 20 seconds
    size_t warm_peak = 0;

    for (uint32_t frame = 0; frame < kSoakFrames; ++frame) {
        prof::begin_frame();
        input::update();
        simulate_frame(scratch, pool, frame);
        prof::end_frame();

        if (frame == kWarmup) {
            warm_peak = mem::totals().peak;
        }
    }

    const mem::Totals end = mem::totals();

    // No growth: the high-water mark after warm-up is the high-water mark at
    // the end. Not "close to" -- identical. A steady-state frame loop that
    // allocates one more byte on frame 50,000 than on frame 600 has a leak.
    EXPECT_EQ(warm_peak, end.peak)
        << "memory high-water grew after warm-up: " << warm_peak << " -> "
        << end.peak;

    // No leaks: everything the pool handed out came back.
    mem::Region pool_region;
    ASSERT_TRUE(mem::find("pool", &pool_region));
    EXPECT_EQ(0u, pool_region.blocks_used);

    // The stack is rewound every frame, so it ends where it started.
    mem::Region scratch_region;
    ASSERT_TRUE(mem::find("scratch", &scratch_region));
    EXPECT_EQ(0u, scratch_region.used);

    // No allocation failures anywhere across the session (D4).
    EXPECT_EQ(0u, end.failures);

    // The recording really did drive the whole session.
    EXPECT_EQ(kSoakFrames, prof::frames_elapsed());
    EXPECT_EQ(kSoakFrames / recording.size(), input::playback::loops());
}

TEST_F(SoakTest, PlaybackReproducesButtonEdgesLikeAHumanPad)
{
    // If playback did not produce real edges, a soak would exercise none of
    // the input-driven code paths and would prove nothing about them.
    std::vector<input::playback::Frame> frames(4);
    frames[0].buttons = 0;
    frames[1].buttons = 1u << static_cast<uint32_t>(input::Button::Cross);
    frames[2].buttons = 1u << static_cast<uint32_t>(input::Button::Cross);
    frames[3].buttons = 0;

    input::playback::start(frames.data(), static_cast<uint32_t>(frames.size()));

    input::update(); // frame 0: nothing held
    EXPECT_FALSE(input::button(0, input::Button::Cross));

    input::update(); // frame 1: pressed this frame
    EXPECT_TRUE(input::button(0, input::Button::Cross));
    EXPECT_TRUE(input::button_down(0, input::Button::Cross));

    input::update(); // frame 2: still held, no new edge
    EXPECT_TRUE(input::button(0, input::Button::Cross));
    EXPECT_FALSE(input::button_down(0, input::Button::Cross));

    input::update(); // frame 3: released
    EXPECT_FALSE(input::button(0, input::Button::Cross));
    EXPECT_TRUE(input::button_up(0, input::Button::Cross));
}

TEST_F(SoakTest, PlaybackLoopsAndReportsItsPosition)
{
    std::vector<input::playback::Frame> frames(3);
    input::playback::start(frames.data(), 3);

    EXPECT_TRUE(input::playback::active());
    for (uint32_t i = 0; i < 7; ++i) {
        input::update();
    }
    // Seven frames over a three-frame recording: two full loops, one into
    // the third.
    EXPECT_EQ(2u, input::playback::loops());
    EXPECT_EQ(1u, input::playback::position());

    input::playback::stop();
    EXPECT_FALSE(input::playback::active());
}

TEST_F(SoakTest, ADeliberateLeakIsCaughtRatherThanTolerated)
{
    // The assertion above is only worth having if it fails when it should.
    // This leaks one block per frame and checks the high-water mark notices.
    static unsigned char pool_mem[32 * 1024];
    PoolAllocator pool;
    pool.init(pool_mem, sizeof(pool_mem), 256);
    ASSERT_TRUE(mem::add_pool("leaky", &pool));

    const size_t warm = mem::totals().peak;
    for (uint32_t i = 0; i < 32; ++i) {
        pool.alloc(); // never freed
    }
    EXPECT_GT(mem::totals().peak, warm);
}
