// Profiler implementation (plan section 9, M13 task 1). See profiler.h for
// the design constraints; the notes here are about the mechanics.
#include "ps2ur/profiler.h"

#include "ps2ur/dma_chain.h"
#include "ps2ur/log.h"
#include "ps2ur/meminfo.h"
#include "ps2ur/platform.h"

#include <cstdio>
#include <cstring>

namespace ps2ur {
namespace prof {
namespace {

struct OpenZone {
    uint32_t id;
    uint64_t start;
    uint32_t child_ticks; // time attributed to nested zones, for exclusive
};

struct State {
    bool enabled = true;

    ZoneStats zones[kMaxZones];
    uint32_t zone_count = 0;

    OpenZone stack[kMaxDepth];
    uint32_t depth = 0;
    uint32_t depth_overflow = 0; // zones dropped because nesting was too deep

    Counters current;
    Counters last;

    uint64_t frame_start = 0;
    uint64_t pipeline_kick = 0;
    bool pipeline_open = false;

    // Baselines so per-frame allocation counts are a difference rather than
    // a running total.
    uint32_t alloc_count_base = 0;
    uint32_t alloc_bytes_base = 0;
    uint32_t alloc_fail_base = 0;

    uint32_t gc_histogram[kGcBuckets] = {};

    // History ring. Zone samples are inclusive ticks, one row per frame.
    Counters hist[kHistoryFrames];
    uint32_t hist_zone[kHistoryFrames][kMaxZones];
    uint32_t hist_head = 0;  // next slot to write
    uint32_t hist_count = 0; // retained frames, saturating at kHistoryFrames

    uint64_t frames = 0;
};

State g;

// Bucket upper edges in milliseconds; the last bucket is unbounded.
const uint32_t kGcEdgesMs[kGcBuckets] = {1, 2, 4, 8, 16, 33, 66, 0};

uint32_t clamp_u32(uint64_t v)
{
    return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(v);
}

} // namespace

void reset()
{
    const bool was_enabled = g.enabled;
    g = State{};
    g.enabled = was_enabled;
}

void set_enabled(bool on) { g.enabled = on; }
bool enabled() { return g.enabled; }

uint32_t zone_id(const char* name)
{
    if (name == nullptr) {
        return kMaxZones;
    }
    for (uint32_t i = 0; i < g.zone_count; ++i) {
        // Compare by content, not by pointer: the same literal in two
        // translation units is not guaranteed to be the same address, and a
        // zone that split in two would quietly halve both halves' numbers.
        if (std::strcmp(g.zones[i].name, name) == 0) {
            return i;
        }
    }
    if (g.zone_count >= kMaxZones) {
        // Loud once, then silent: a table this size overflowing is a coding
        // mistake, not a runtime condition, and spamming a log per frame
        // would itself distort the profile.
        static bool warned = false;
        if (!warned) {
            warned = true;
            log(LogLevel::Warn,
                "prof: zone table full (%u); '%s' and later zones are not "
                "measured",
                static_cast<unsigned>(kMaxZones), name);
        }
        return kMaxZones;
    }
    const uint32_t id = g.zone_count++;
    g.zones[id] = ZoneStats{};
    g.zones[id].name = name;
    return id;
}

void zone_begin(uint32_t id)
{
    if (!g.enabled || id >= g.zone_count) {
        return;
    }
    if (g.depth >= kMaxDepth) {
        g.depth_overflow++;
        return;
    }
    OpenZone& z = g.stack[g.depth++];
    z.id = id;
    z.child_ticks = 0;
    z.start = platform::now_ticks();
}

void zone_end(uint32_t id)
{
    if (!g.enabled || id >= g.zone_count || g.depth == 0) {
        return;
    }
    // Tolerate a mismatched end rather than corrupting the stack: unwinding
    // to the matching frame keeps a stray end from silently reassigning
    // every enclosing zone's time.
    uint32_t at = g.depth;
    while (at > 0 && g.stack[at - 1].id != id) {
        --at;
    }
    if (at == 0) {
        return;
    }
    g.depth = at - 1;

    const OpenZone& z = g.stack[g.depth];
    const uint32_t elapsed = clamp_u32(platform::now_ticks() - z.start);

    ZoneStats& s = g.zones[id];
    if (s.calls == 0) {
        s.frames++;
    }
    s.calls++;
    s.inclusive += elapsed;
    s.exclusive += elapsed - (z.child_ticks > elapsed ? elapsed : z.child_ticks);

    if (g.depth > 0) {
        g.stack[g.depth - 1].child_ticks += elapsed;
    }
}

uint32_t zone_count() { return g.zone_count; }

const ZoneStats& zone(uint32_t id)
{
    static const ZoneStats empty{};
    return id < g.zone_count ? g.zones[id] : empty;
}

void mark_pipeline_kick()
{
    if (!g.enabled) {
        return;
    }
    // First kick of the frame opens the window; later kicks extend it.
    if (!g.pipeline_open) {
        g.pipeline_kick = platform::now_ticks();
        g.pipeline_open = true;
    }
}

void record_dma(const gfx::DmaChain& chain)
{
    if (!g.enabled) {
        return;
    }
    const gfx::DmaChain::Stats& s = chain.stats();
    g.current.dma_build_ticks = clamp_u32(s.build_ticks);
    g.current.dma_kick_ticks = clamp_u32(s.kick_ticks);
    g.current.dma_wait_ticks = clamp_u32(s.wait_ticks);
    g.current.dma_batches = s.batches;
    g.current.dma_qwords = s.qwords;
    g.current.dma_overflows = s.overflows;

    if (g.pipeline_open) {
        g.current.pipeline_busy_estimate =
            clamp_u32(platform::now_ticks() - g.pipeline_kick);
        g.pipeline_open = false;
    }
}

void record_gc_pause(uint32_t ticks)
{
    if (!g.enabled) {
        return;
    }
    g.current.gc_pauses++;
    g.current.gc_pause_ticks += ticks;

    const float ms = ticks_to_ms(ticks);
    uint32_t bucket = kGcBuckets - 1;
    for (uint32_t i = 0; i + 1 < kGcBuckets; ++i) {
        if (ms < static_cast<float>(kGcEdgesMs[i])) {
            bucket = i;
            break;
        }
    }
    g.gc_histogram[bucket]++;
}

const Counters& counters() { return g.current; }
const Counters& last_counters() { return g.last; }
const uint32_t* gc_histogram() { return g.gc_histogram; }

uint32_t gc_bucket_edge_ms(uint32_t bucket)
{
    return bucket < kGcBuckets ? kGcEdgesMs[bucket] : 0u;
}

void begin_frame()
{
    if (!g.enabled) {
        return;
    }
    for (uint32_t i = 0; i < g.zone_count; ++i) {
        g.zones[i].inclusive = 0;
        g.zones[i].exclusive = 0;
        g.zones[i].calls = 0;
    }
    g.current = Counters{};
    g.depth = 0;

    // Allocation activity is a difference of counters the allocators already
    // keep, so per-frame allocation counts cost one pass over the registry
    // rather than a hook on every allocation.
    const mem::Totals t = mem::totals();
    g.alloc_count_base = t.allocs;
    g.alloc_bytes_base = static_cast<uint32_t>(t.used);
    g.alloc_fail_base = t.failures;

    g.frame_start = platform::now_ticks();
}

void end_frame()
{
    if (!g.enabled) {
        return;
    }
    g.current.frame_ticks = clamp_u32(platform::now_ticks() - g.frame_start);

    const mem::Totals t = mem::totals();
    g.current.alloc_count =
        t.allocs > g.alloc_count_base ? t.allocs - g.alloc_count_base : 0u;
    // Bytes can legitimately fall within a frame (a stack rewind, a pool
    // free), so this is the net growth and clamps at zero rather than
    // wrapping into a nonsense number.
    const uint32_t used_now = static_cast<uint32_t>(t.used);
    g.current.alloc_bytes =
        used_now > g.alloc_bytes_base ? used_now - g.alloc_bytes_base : 0u;
    g.current.alloc_failures =
        t.failures > g.alloc_fail_base ? t.failures - g.alloc_fail_base : 0u;

    for (uint32_t i = 0; i < g.zone_count; ++i) {
        ZoneStats& s = g.zones[i];
        if (s.calls == 0) {
            continue;
        }
        if (s.inclusive < s.min_inclusive) {
            s.min_inclusive = s.inclusive;
        }
        if (s.inclusive > s.max_inclusive) {
            s.max_inclusive = s.inclusive;
        }
        s.total_inclusive += s.inclusive;
    }

    g.hist[g.hist_head] = g.current;
    for (uint32_t i = 0; i < kMaxZones; ++i) {
        g.hist_zone[g.hist_head][i] = i < g.zone_count ? g.zones[i].inclusive : 0u;
    }
    g.hist_head = (g.hist_head + 1) % kHistoryFrames;
    if (g.hist_count < kHistoryFrames) {
        g.hist_count++;
    }

    g.last = g.current;
    g.frames++;
}

uint32_t history_count() { return g.hist_count; }

const Counters& history(uint32_t index)
{
    static const Counters empty{};
    if (index >= g.hist_count) {
        return empty;
    }
    // Oldest retained frame first.
    const uint32_t start =
        (g.hist_head + kHistoryFrames - g.hist_count) % kHistoryFrames;
    return g.hist[(start + index) % kHistoryFrames];
}

uint32_t history_zone(uint32_t index, uint32_t zone_index)
{
    if (index >= g.hist_count || zone_index >= kMaxZones) {
        return 0;
    }
    const uint32_t start =
        (g.hist_head + kHistoryFrames - g.hist_count) % kHistoryFrames;
    return g.hist_zone[(start + index) % kHistoryFrames][zone_index];
}

uint64_t frames_elapsed() { return g.frames; }

float ticks_to_ms(uint64_t ticks)
{
    const uint64_t hz = platform::ticks_per_second();
    if (hz == 0) {
        return 0.0f;
    }
    return static_cast<float>(ticks) * 1000.0f / static_cast<float>(hz);
}

bool dump_csv(const char* path)
{
    FILE* f = std::fopen(path, "w");
    if (f == nullptr) {
        log(LogLevel::Error, "prof: cannot open '%s' for the CSV dump", path);
        return false;
    }
    // Milliseconds, not ticks: a CSV that needs the reader to know the EE's
    // clock rate to mean anything is a trap for whoever opens it in a year.
    std::fprintf(f, "frame,frame_ms,dma_build_ms,dma_kick_ms,dma_wait_ms,"
                    "pipeline_busy_ms,dma_batches,dma_qwords,dma_overflows,"
                    "alloc_count,alloc_bytes,alloc_failures,gc_pauses,"
                    "gc_pause_ms");
    for (uint32_t z = 0; z < g.zone_count; ++z) {
        std::fprintf(f, ",%s_ms", g.zones[z].name);
    }
    std::fprintf(f, "\n");

    const uint32_t n = g.hist_count;
    for (uint32_t i = 0; i < n; ++i) {
        const Counters& c = history(i);
        std::fprintf(f,
                     "%u,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%u,%u,%u,%u,%.3f",
                     static_cast<unsigned>(i),
                     static_cast<double>(ticks_to_ms(c.frame_ticks)),
                     static_cast<double>(ticks_to_ms(c.dma_build_ticks)),
                     static_cast<double>(ticks_to_ms(c.dma_kick_ticks)),
                     static_cast<double>(ticks_to_ms(c.dma_wait_ticks)),
                     static_cast<double>(ticks_to_ms(c.pipeline_busy_estimate)),
                     static_cast<unsigned>(c.dma_batches),
                     static_cast<unsigned>(c.dma_qwords),
                     static_cast<unsigned>(c.dma_overflows),
                     static_cast<unsigned>(c.alloc_count),
                     static_cast<unsigned>(c.alloc_bytes),
                     static_cast<unsigned>(c.alloc_failures),
                     static_cast<unsigned>(c.gc_pauses),
                     static_cast<double>(ticks_to_ms(c.gc_pause_ticks)));
        for (uint32_t z = 0; z < g.zone_count; ++z) {
            std::fprintf(f, ",%.3f",
                         static_cast<double>(ticks_to_ms(history_zone(i, z))));
        }
        std::fprintf(f, "\n");
    }
    std::fclose(f);
    log(LogLevel::Info, "prof: wrote %u frames to %s", static_cast<unsigned>(n),
        path);
    return true;
}

void log_summary()
{
    log(LogLevel::Info, "[prof] frames %u retained %u",
        static_cast<unsigned>(g.frames), static_cast<unsigned>(g.hist_count));

    // Frame time percentiles matter more than the mean for a 30 fps target:
    // one frame in sixty over budget is a visible hitch that a mean hides.
    uint32_t worst = 0;
    uint64_t total = 0;
    uint32_t dropped = 0;
    for (uint32_t i = 0; i < g.hist_count; ++i) {
        const uint32_t t = history(i).frame_ticks;
        total += t;
        if (t > worst) {
            worst = t;
        }
        if (ticks_to_ms(t) > kDroppedFrameMs) {
            dropped++;
        }
    }
    const uint32_t mean = g.hist_count ? clamp_u32(total / g.hist_count) : 0;
    const float mean_ms = ticks_to_ms(mean);
    log(LogLevel::Info,
        "[prof] frame ms mean %.2f worst %.2f fps %.2f dropped %u/%u",
        static_cast<double>(mean_ms), static_cast<double>(ticks_to_ms(worst)),
        static_cast<double>(mean_ms > 0.0f ? 1000.0f / mean_ms : 0.0f),
        static_cast<unsigned>(dropped), static_cast<unsigned>(g.hist_count));

    for (uint32_t i = 0; i < g.zone_count; ++i) {
        const ZoneStats& s = g.zones[i];
        if (s.frames == 0) {
            continue;
        }
        log(LogLevel::Info,
            "[prof] zone %-18s min %6.3f mean %6.3f max %6.3f ms",
            s.name, static_cast<double>(ticks_to_ms(s.min_inclusive)),
            static_cast<double>(ticks_to_ms(s.mean_inclusive())),
            static_cast<double>(ticks_to_ms(s.max_inclusive)));
    }

    const Counters& c = g.last;
    log(LogLevel::Info,
        "[prof] dma build %.3f kick %.3f wait %.3f pipeline %.3f ms "
        "batches %u qwords %u overflows %u",
        static_cast<double>(ticks_to_ms(c.dma_build_ticks)),
        static_cast<double>(ticks_to_ms(c.dma_kick_ticks)),
        static_cast<double>(ticks_to_ms(c.dma_wait_ticks)),
        static_cast<double>(ticks_to_ms(c.pipeline_busy_estimate)),
        static_cast<unsigned>(c.dma_batches),
        static_cast<unsigned>(c.dma_qwords),
        static_cast<unsigned>(c.dma_overflows));

    for (uint32_t b = 0; b < kGcBuckets; ++b) {
        if (g.gc_histogram[b] == 0) {
            continue;
        }
        if (kGcEdgesMs[b] == 0) {
            log(LogLevel::Info, "[prof] gc pause >=%u ms: %u",
                static_cast<unsigned>(kGcEdgesMs[b - 1]),
                static_cast<unsigned>(g.gc_histogram[b]));
        } else {
            log(LogLevel::Info, "[prof] gc pause <%u ms: %u",
                static_cast<unsigned>(kGcEdgesMs[b]),
                static_cast<unsigned>(g.gc_histogram[b]));
        }
    }

    if (g.depth_overflow > 0) {
        log(LogLevel::Warn, "[prof] %u zone begins exceeded depth %u",
            static_cast<unsigned>(g.depth_overflow),
            static_cast<unsigned>(kMaxDepth));
    }
}

} // namespace prof
} // namespace ps2ur
