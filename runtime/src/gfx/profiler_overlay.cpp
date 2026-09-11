// Profiler overlay implementation (plan section 9, M13 task 1).
#include "ps2ur/profiler_overlay.h"

#include "ps2ur/gs_device.h"
#include "ps2ur/gs_overlay.h"
#include "ps2ur/gs_vram.h"
#include "ps2ur/meminfo.h"

#include <cstring>
#include "ps2ur/profiler.h"

namespace ps2ur {
namespace prof {
namespace {

Page g_page = Page::Off;

// The vsync period is the line everything on the Frame page is drawn
// against: a bar reaching it is a frame that used its whole slot, and a bar
// past kDroppedFrameMs is a flip that was missed (plan section 15.3).
constexpr float kFrameBudgetMs = kVsyncPeriodMs;

// A graph taller than the budget line so an over-budget frame has somewhere
// to go visually rather than clipping flat at the top.
constexpr float kGraphTopMs = 50.0f;

struct Rgb {
    uint8_t r, g, b;
};

// Green under budget, amber approaching it, red over. The thresholds are
// fractions of the budget rather than absolute times so the same code reads
// correctly if the target ever moves.
Rgb budget_colour(float value_ms, float budget_ms)
{
    const float f = budget_ms <= 0.0f ? 0.0f : value_ms / budget_ms;
    if (f > 1.0f) {
        return Rgb{0xE0, 0x40, 0x40};
    }
    if (f > 0.8f) {
        return Rgb{0xE0, 0xC0, 0x40};
    }
    return Rgb{0x50, 0xD0, 0x70};
}

void panel(gfx::GsDevice& d, gfx::DebugOverlay& o, int32_t x, int32_t y,
           int32_t w, int32_t h)
{
    // Semi-transparent so the frame underneath stays visible: an overlay you
    // cannot see past hides the thing you are profiling.
    o.fill_rect(d, x, y, w, h, 0x08, 0x0A, 0x12, 0x60);
}

void draw_frame_page(gfx::GsDevice& d, gfx::DebugOverlay& o, int32_t sw)
{
    const Counters& c = last_counters();
    const uint32_t n = history_count();

    float worst = 0.0f;
    float total = 0.0f;
    uint32_t over = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const float ms = ticks_to_ms(history(i).frame_ticks);
        total += ms;
        if (ms > worst) {
            worst = ms;
        }
        if (ms > kDroppedFrameMs) {
            over++;
        }
    }
    const float mean = n ? total / static_cast<float>(n) : 0.0f;
    const float fps = mean > 0.0001f ? 1000.0f / mean : 0.0f;

    const int32_t x = 8, y = 8, w = sw - 16, h = 150;
    panel(d, o, x, y, w, h);

    o.set_colour(0xE6, 0xE7, 0xEC);
    o.printf_at(d, x + 8, y + 6, "FRAME  %.1f fps   mean %.2f ms   worst %.2f ms",
                static_cast<double>(fps), static_cast<double>(mean),
                static_cast<double>(worst));

    const Rgb over_col = over > 0 ? Rgb{0xE0, 0x40, 0x40} : Rgb{0x50, 0xD0, 0x70};
    o.set_colour(over_col.r, over_col.g, over_col.b);
    o.printf_at(d, x + 8, y + 18, "dropped %u of %u frames",
                static_cast<unsigned>(over), static_cast<unsigned>(n));

    // The graph. One column per retained frame, scaled against kGraphTopMs.
    const int32_t gx = x + 8, gy = y + 34;
    const int32_t gw = w - 16, gh = 74;
    o.fill_rect(d, gx, gy, gw, gh, 0x04, 0x05, 0x0A, 0x70);

    const int32_t col_w = n > 0 ? (gw / static_cast<int32_t>(n)) : 1;
    const int32_t step = col_w > 0 ? col_w : 1;
    for (uint32_t i = 0; i < n; ++i) {
        const Counters& f = history(i);
        const float ms = ticks_to_ms(f.frame_ticks);
        int32_t bar = static_cast<int32_t>((ms / kGraphTopMs) *
                                           static_cast<float>(gh));
        if (bar > gh) {
            bar = gh;
        }
        if (bar < 1) {
            bar = 1;
        }
        const Rgb col = budget_colour(ms, kFrameBudgetMs);
        const int32_t bx = gx + static_cast<int32_t>(i) * step;
        o.fill_rect(d, bx, gy + gh - bar, step > 1 ? step - 1 : 1, bar, col.r,
                    col.g, col.b, 0x80);

        // The share of the frame spent blocked on the pipeline, drawn inside
        // the same column in a dimmer shade. A tall bar that is mostly this
        // colour means the EE is waiting, not working.
        const float wait_ms = ticks_to_ms(f.dma_wait_ticks);
        int32_t wbar = static_cast<int32_t>((wait_ms / kGraphTopMs) *
                                            static_cast<float>(gh));
        if (wbar > bar) {
            wbar = bar;
        }
        if (wbar > 0) {
            o.fill_rect(d, bx, gy + gh - wbar, step > 1 ? step - 1 : 1, wbar,
                        0x30, 0x50, 0x90, 0x80);
        }
    }

    // Budget line across the graph.
    const int32_t budget_y =
        gy + gh - static_cast<int32_t>((kFrameBudgetMs / kGraphTopMs) *
                                       static_cast<float>(gh));
    o.fill_rect(d, gx, budget_y, gw, 1, 0xE0, 0xC0, 0x40, 0x80);

    o.set_colour(0x9A, 0xA0, 0xAE);
    o.printf_at(d, x + 8, y + 114,
                "dma build %.2f kick %.2f wait %.2f  pipeline %.2f ms",
                static_cast<double>(ticks_to_ms(c.dma_build_ticks)),
                static_cast<double>(ticks_to_ms(c.dma_kick_ticks)),
                static_cast<double>(ticks_to_ms(c.dma_wait_ticks)),
                static_cast<double>(ticks_to_ms(c.pipeline_busy_estimate)));
    o.printf_at(d, x + 8, y + 126, "batches %u  qwords %u  overflows %u",
                static_cast<unsigned>(c.dma_batches),
                static_cast<unsigned>(c.dma_qwords),
                static_cast<unsigned>(c.dma_overflows));
    o.printf_at(d, x + 8, y + 138, "allocs %u (%u B)  gc %u pauses %.2f ms",
                static_cast<unsigned>(c.alloc_count),
                static_cast<unsigned>(c.alloc_bytes),
                static_cast<unsigned>(c.gc_pauses),
                static_cast<double>(ticks_to_ms(c.gc_pause_ticks)));
}

void draw_zones_page(gfx::GsDevice& d, gfx::DebugOverlay& o, int32_t sw)
{
    const uint32_t zones = zone_count();
    const int32_t row_h = 10;
    const int32_t x = 8, y = 8, w = sw - 16;
    const int32_t h = 34 + static_cast<int32_t>(zones) * row_h;
    panel(d, o, x, y, w, h);

    o.set_colour(0xE6, 0xE7, 0xEC);
    o.printf_at(d, x + 8, y + 6, "ZONES  ms per frame   min / mean / max");

    // Bars are scaled against the frame budget, not against the largest zone:
    // a zone taking half the budget should look like half the budget, not
    // like a full bar just because nothing else is slower.
    const int32_t bar_x = x + 8 + 8 * 34;
    const int32_t bar_w = w - (bar_x - x) - 8;

    int32_t row = y + 20;
    for (uint32_t i = 0; i < zones; ++i) {
        const ZoneStats& s = zone(i);
        if (s.frames == 0) {
            continue;
        }
        const float mean = ticks_to_ms(s.mean_inclusive());
        const float mx = ticks_to_ms(s.max_inclusive);
        const Rgb col = budget_colour(mean, kFrameBudgetMs);

        o.set_colour(0xC8, 0xCC, 0xD6);
        o.printf_at(d, x + 8, row, "%-12s %5.2f %5.2f %5.2f", s.name,
                    static_cast<double>(ticks_to_ms(s.min_inclusive)),
                    static_cast<double>(mean), static_cast<double>(mx));

        int32_t bw = static_cast<int32_t>((mean / kFrameBudgetMs) *
                                          static_cast<float>(bar_w));
        if (bw > bar_w) {
            bw = bar_w;
        }
        if (bw > 0) {
            o.fill_rect(d, bar_x, row + 1, bw, 6, col.r, col.g, col.b, 0x80);
        }
        row += row_h;
    }
}

void draw_memory_page(gfx::GsDevice& d, gfx::DebugOverlay& o, int32_t sw)
{
    const uint32_t regions = mem::count();
    const int32_t row_h = 10;
    const int32_t x = 8, y = 8, w = sw - 16;
    const int32_t h = 46 + static_cast<int32_t>(regions) * row_h;
    panel(d, o, x, y, w, h);

    o.set_colour(0xE6, 0xE7, 0xEC);
    o.printf_at(d, x + 8, y + 6, "MEMORY  KB used / peak / capacity");

    const int32_t bar_x = x + 8 + 8 * 34;
    const int32_t bar_w = w - (bar_x - x) - 8;

    int32_t row = y + 20;
    for (uint32_t i = 0; i < regions; ++i) {
        const mem::Region r = mem::snapshot(i);
        const float frac =
            r.capacity == 0 ? 0.0f
                            : static_cast<float>(r.peak) /
                                  static_cast<float>(r.capacity);
        // Anything over 80% of its region is worth seeing before it is over
        // 100% of it, which on this machine is a crash rather than a swap.
        const Rgb col = budget_colour(frac, 1.0f);

        o.set_colour(0xC8, 0xCC, 0xD6);
        o.printf_at(d, x + 8, row, "%-12s %5u %5u %5u", r.name,
                    static_cast<unsigned>(r.used / 1024),
                    static_cast<unsigned>(r.peak / 1024),
                    static_cast<unsigned>(r.capacity / 1024));

        int32_t bw = static_cast<int32_t>(frac * static_cast<float>(bar_w));
        if (bw > bar_w) {
            bw = bar_w;
        }
        if (bw > 0) {
            o.fill_rect(d, bar_x, row + 1, bw, 6, col.r, col.g, col.b, 0x80);
        }
        row += row_h;
    }

    const mem::Totals t = mem::totals();
    o.set_colour(0xE6, 0xE7, 0xEC);
    o.printf_at(d, x + 8, row + 4, "TOTAL %u KB used, %u KB peak, %u failures",
                static_cast<unsigned>(t.used / 1024),
                static_cast<unsigned>(t.peak / 1024),
                static_cast<unsigned>(t.failures));
    o.printf_at(d, x + 8, row + 16, "heap idle %.1f%%",
                static_cast<double>(mem::heap_fragmentation() * 100.0f));
}

// The VRAM page: what the [vram] boot log says, live, plus every
// allocation by name and an occupancy map. Textures that draw garbled or
// vanish are nearly always a VRAM story -- an upload that did not fit, two
// buffers that overlap, a CLUT in the wrong page -- and the boot log
// scrolls away on real hardware. One char of the map is 8 pages (64 KB).
void draw_vram_page(gfx::GsDevice& d, gfx::DebugOverlay& o, int32_t sw)
{
    const gfx::VramAllocator& vram = d.vram();
    const uint32_t kb = gfx::kPageBytes / 1024u;

    // Frame buffers are the device's own; everything else is content.
    uint32_t frame_pages = 0, content_pages = 0, records = 0;
    for (uint32_t i = 0; i < gfx::VramAllocator::kMaxAllocs; ++i) {
        uint32_t page, count;
        const char* name;
        if (!vram.record(i, page, count, name)) {
            continue;
        }
        ++records;
        const bool is_frame = std::strncmp(name, "colour", 6) == 0 ||
                              std::strncmp(name, "depth", 5) == 0;
        (is_frame ? frame_pages : content_pages) += count;
    }

    const int32_t row_h = 10;
    const int32_t x = 8, y = 8, w = sw - 16;
    // Header, totals, map, column titles, then one row per record; the
    // panel stops at the screen so a long list is clipped, not wrapped.
    int32_t rows = static_cast<int32_t>(records);
    const int32_t max_rows = (448 - 8 - 64) / row_h;
    if (rows > max_rows) {
        rows = max_rows;
    }
    panel(d, o, x, y, w, 64 + rows * row_h);

    o.set_colour(0xE6, 0xE7, 0xEC);
    o.printf_at(d, x + 8, y + 6, "VRAM  %u KB: frame %u  content %u  free %u",
                static_cast<unsigned>(gfx::kPageCount * kb),
                static_cast<unsigned>(frame_pages * kb),
                static_cast<unsigned>(content_pages * kb),
                static_cast<unsigned>(vram.free_pages() * kb));
    o.set_colour(0xC8, 0xCC, 0xD6);
    o.printf_at(d, x + 8, y + 16,
                "CLUTs %u in %u pages  largest free run %u pages (%u KB)",
                static_cast<unsigned>(vram.clut_slots_used()),
                static_cast<unsigned>(vram.clut_pages()),
                static_cast<unsigned>(vram.largest_free_run()),
                static_cast<unsigned>(vram.largest_free_run() * kb));

    // Occupancy map: '.' free, '+' partly used, '#' full, 64 KB per char.
    char map[gfx::kPageCount / 8 + 1];
    for (uint32_t g = 0; g < gfx::kPageCount / 8; ++g) {
        uint32_t used = 0;
        for (uint32_t p = g * 8; p < g * 8 + 8; ++p) {
            used += vram.page_used(p) ? 1u : 0u;
        }
        map[g] = used == 0 ? '.' : used == 8 ? '#' : '+';
    }
    map[gfx::kPageCount / 8] = '\0';
    o.set_colour(0x50, 0xD0, 0x70);
    o.printf_at(d, x + 8, y + 28, "%s", map);

    o.set_colour(0xE6, 0xE7, 0xEC);
    o.printf_at(d, x + 8, y + 42, "page   KB  name");
    int32_t row = y + 54;
    int32_t shown = 0;
    for (uint32_t i = 0; i < gfx::VramAllocator::kMaxAllocs && shown < rows;
         ++i) {
        uint32_t page, count;
        const char* name;
        if (!vram.record(i, page, count, name)) {
            continue;
        }
        const bool is_frame = std::strncmp(name, "colour", 6) == 0 ||
                              std::strncmp(name, "depth", 5) == 0;
        if (is_frame) {
            o.set_colour(0x90, 0x94, 0xA0);
        } else if (count == 0) {
            o.set_colour(0x70, 0xC0, 0xE0); // a CLUT slot inside a page
        } else {
            o.set_colour(0xE6, 0xE7, 0xEC);
        }
        o.printf_at(d, x + 8, row, "%3u..%-3u %4u  %s",
                    static_cast<unsigned>(page),
                    static_cast<unsigned>(count > 0 ? page + count - 1u : page),
                    static_cast<unsigned>(count * kb), name);
        row += row_h;
        ++shown;
    }
    if (shown < static_cast<int32_t>(records)) {
        o.set_colour(0xC8, 0xCC, 0xD6);
        o.printf_at(d, x + 8, row, "... %u more",
                    static_cast<unsigned>(records - shown));
    }
}

} // namespace

void set_page(Page p) { g_page = p; }
Page page() { return g_page; }

void next_page()
{
    const uint8_t n = static_cast<uint8_t>(Page::Count);
    g_page = static_cast<Page>((static_cast<uint8_t>(g_page) + 1u) % n);
}

void draw_overlay(gfx::GsDevice& device, gfx::DebugOverlay& overlay,
                  int32_t screen_w, int32_t screen_h)
{
    (void)screen_h;
    if (g_page == Page::Off || !overlay.initialized()) {
        return;
    }
    switch (g_page) {
        case Page::Frame:
            draw_frame_page(device, overlay, screen_w);
            break;
        case Page::Zones:
            draw_zones_page(device, overlay, screen_w);
            break;
        case Page::Memory:
            draw_memory_page(device, overlay, screen_w);
            break;
        case Page::Vram:
            draw_vram_page(device, overlay, screen_w);
            break;
        default:
            break;
    }
}

} // namespace prof
} // namespace ps2ur
