// On-target profiler (plan section 9, M13 task 1): EE cycle counters around
// named zones, the VU1/GS pipeline window, DMA wait, per-frame allocation
// counts and a GC pause histogram. Surfaced as a live overlay and as a CSV
// dumped over host: for offline analysis (tools/profiler/view.py).
//
// Four constraints shaped this file:
//
//  1. Zero allocation. Zones live in a fixed table, registered on first use.
//     A profiler that allocates changes the thing it is measuring, and on a
//     32 MB machine it also competes with the game for the budget it is
//     supposed to be policing.
//  2. Cheap enough to leave on. A zone costs two reads of the EE cycle
//     counter (mfc0 $9, which is what platform::now_ticks already does) plus
//     a handful of adds. Measured overhead is in the profiler's own
//     self-test; if it ever stops being negligible the number is visible
//     rather than assumed.
//  3. Honest labels. Numbers that are estimates say "estimate" in their
//     name. Plan section 15.3 says to profile the EE first, always, because
//     VU1 and the GS run concurrently with it -- so a profiler that quietly
//     presented a guess as a measurement would send you to the wrong side of
//     the machine.
//  4. Nesting is real. Zones nest, so each one reports inclusive time (with
//     children) and exclusive time (without). Exclusive is what finds the
//     culprit; inclusive is what explains the shape of a frame.
//
// The whole thing compiles to nothing when PS2UR_PROFILE is 0, so a release
// build carries no instrumentation at all.
#pragma once

#include <cstddef>
#include <cstdint>

// Instrumentation is compiled in by default. Set to 0 for a build that must
// carry no profiler cost whatsoever.
#ifndef PS2UR_PROFILE
#define PS2UR_PROFILE 1
#endif

namespace ps2ur {

namespace gfx {
class DmaChain;
}

namespace prof {

// Fixed capacities. Sized so the whole profiler, history included, fits in
// well under 32 KB: it is charged to the runtime's slack, not to a budget
// the game is entitled to (plan section 15.1).
inline constexpr uint32_t kMaxZones = 32;
inline constexpr uint32_t kHistoryFrames = 120; // 4 seconds at 30 fps
inline constexpr uint32_t kMaxDepth = 16;
// GC pause buckets, in milliseconds: [0,1) [1,2) [2,4) [4,8) [8,16) [16,33)
// [33,66) [66,inf). The 33 ms edge matters because a pause past it has
// certainly dropped a frame (plan section 15.3 budgets 2 ms amortised).
inline constexpr uint32_t kGcBuckets = 8;

// One NTSC frame is two fields at 59.94 Hz. A vsync-locked frame therefore
// measures 33.37 ms and is ON budget, not over it: comparing against a round
// 33.3 would report every healthy frame as a miss.
inline constexpr float kVsyncPeriodMs = 33.37f;

// What actually matters is whether the flip was missed, and a missed flip
// costs a whole field. This threshold sits between one period and two, so it
// is immune to a fraction of a percent of clock calibration error while still
// catching every genuinely dropped frame.
inline constexpr float kDroppedFrameMs = 40.0f;

struct ZoneStats {
    const char* name = nullptr;
    // This frame.
    uint32_t inclusive = 0; // ticks including children
    uint32_t exclusive = 0; // ticks excluding children
    uint32_t calls = 0;
    // Across the run.
    uint32_t min_inclusive = 0xFFFFFFFFu;
    uint32_t max_inclusive = 0;
    uint64_t total_inclusive = 0;
    uint32_t frames = 0; // frames in which this zone ran at least once

    // Mean inclusive ticks over the frames the zone actually ran. Returns 0
    // before it has ever run, rather than dividing by zero.
    uint32_t mean_inclusive() const
    {
        return frames == 0 ? 0u
                           : static_cast<uint32_t>(total_inclusive / frames);
    }
};

// Everything measured per frame that is not a zone.
struct Counters {
    uint32_t frame_ticks = 0; // wall time of the frame, EE ticks

    // DMA / VU1 / GS. build+kick are EE occupancy; wait is the EE blocked on
    // the pipeline, which is the number that says "the EE is not the
    // bottleneck" (plan section 15.3).
    uint32_t dma_build_ticks = 0;
    uint32_t dma_kick_ticks = 0;
    uint32_t dma_wait_ticks = 0;
    uint32_t dma_batches = 0;
    uint32_t dma_qwords = 0;
    uint32_t dma_overflows = 0;

    // The window between kicking the chain and the pipeline going idle. VU1
    // and the GS are working somewhere inside it; the split between them is
    // not separately observable without hardware counters the EE does not
    // have, which is why this is one number and is named as an estimate.
    uint32_t pipeline_busy_estimate = 0;

    // Allocation activity, summed across every registered allocator. Free,
    // because it is a difference of counters the allocators already keep.
    uint32_t alloc_count = 0;
    uint32_t alloc_bytes = 0;
    uint32_t alloc_failures = 0;

    // Managed collector, reported by whoever runs it (record_gc_pause).
    uint32_t gc_pauses = 0;
    uint32_t gc_pause_ticks = 0;
};

// ---- lifecycle ---------------------------------------------------------

// Clears every zone, counter and history entry. Safe to call again to start
// a fresh measurement window (the overlay's "reset" and the soak test's
// per-segment reset both use it).
void reset();

// Enable/disable measurement without recompiling. Disabled zones cost one
// predictable branch. Enabled by default.
void set_enabled(bool on);
bool enabled();

void begin_frame();
void end_frame();

// ---- zones -------------------------------------------------------------

// Registers 'name' on first call and returns a stable id for the process
// lifetime. 'name' must have static storage duration: the table keeps the
// pointer rather than copying, because copying would mean owning memory.
// Returns kMaxZones when the table is full, which every entry point below
// treats as a no-op rather than a crash.
uint32_t zone_id(const char* name);

void zone_begin(uint32_t id);
void zone_end(uint32_t id);

uint32_t zone_count();
const ZoneStats& zone(uint32_t id);

// ---- counters ----------------------------------------------------------

// Folds a DMA chain's per-frame statistics into this frame's counters, and
// closes the pipeline-busy window. Call once per frame after the chain has
// been waited on.
void record_dma(const gfx::DmaChain& chain);

// Marks the moment the chain was kicked, opening the pipeline-busy window.
void mark_pipeline_kick();

// Reports a managed collector pause. Called from wherever the collector
// actually runs; the profiler does not depend on any particular collector.
void record_gc_pause(uint32_t ticks);

const Counters& counters();          // this frame, so far
const Counters& last_counters();     // the frame just completed
const uint32_t* gc_histogram();      // kGcBuckets entries, cumulative
uint32_t gc_bucket_edge_ms(uint32_t bucket); // upper edge, 0 for the last

// ---- history -----------------------------------------------------------

// Ring of completed frames, newest last. 'index' 0 is the oldest retained
// frame. history_count() saturates at kHistoryFrames.
uint32_t history_count();
const Counters& history(uint32_t index);
// Inclusive ticks for 'zone_index' in the same retained frame.
uint32_t history_zone(uint32_t index, uint32_t zone_index);

uint64_t frames_elapsed(); // total frames since reset()

// ---- reporting ---------------------------------------------------------

// Writes the retained history as CSV to 'path' (typically
// "host:profile.csv"). One row per frame: counters, then one column per
// registered zone. Returns false if the file could not be opened.
bool dump_csv(const char* path);

// Writes a compact human-readable summary to the log: per-zone min/mean/max
// in milliseconds, the counters, and the GC histogram. This is what CI
// parses, so the format is stable and every line is prefixed [prof].
void log_summary();

// Ticks to milliseconds, using the platform's tick rate. Float, never
// double (plan section 3.1).
float ticks_to_ms(uint64_t ticks);

// ---- scope helper ------------------------------------------------------

class ScopedZone {
public:
    explicit ScopedZone(uint32_t id) : m_id(id) { zone_begin(m_id); }
    ~ScopedZone() { zone_end(m_id); }

    ScopedZone(const ScopedZone&) = delete;
    ScopedZone& operator=(const ScopedZone&) = delete;

private:
    uint32_t m_id;
};

} // namespace prof
} // namespace ps2ur

// Times the enclosing scope under a literal name. The id is resolved once
// per call site (function-local static), so the steady-state cost is the two
// counter reads and nothing else.
#if PS2UR_PROFILE
#define PS2UR_PROFILE_ZONE_TOKEN2(a, b) a##b
#define PS2UR_PROFILE_ZONE_TOKEN(a, b) PS2UR_PROFILE_ZONE_TOKEN2(a, b)
#define PS2UR_PROFILE_ZONE(name)                                              \
    static const uint32_t PS2UR_PROFILE_ZONE_TOKEN(ps2ur_zone_, __LINE__) =   \
        ::ps2ur::prof::zone_id(name);                                         \
    ::ps2ur::prof::ScopedZone PS2UR_PROFILE_ZONE_TOKEN(ps2ur_scope_, __LINE__)( \
        PS2UR_PROFILE_ZONE_TOKEN(ps2ur_zone_, __LINE__))
#else
#define PS2UR_PROFILE_ZONE(name) ((void)0)
#endif
