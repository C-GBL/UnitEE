#include "ps2ur/render_queue.h"

namespace ps2ur {
namespace gfx {

uint64_t RenderQueue::make_key(uint32_t pass, uint32_t kind,
                               uint32_t texture_plus_one, float depth01,
                               uint32_t submission_index)
{
    if (depth01 < 0.0f) {
        depth01 = 0.0f;
    }
    if (depth01 > 1.0f) {
        depth01 = 1.0f;
    }
    uint32_t depth = static_cast<uint32_t>(depth01 * 16777215.0f); // 2^24-1
    if (pass != 0) {
        depth = 16777215u - depth; // transparent: back-to-front
    }
    return (static_cast<uint64_t>(pass & 3u) << 62) |
           (static_cast<uint64_t>(kind & 7u) << 59) |
           (static_cast<uint64_t>(texture_plus_one & 0xFFu) << 51) |
           (static_cast<uint64_t>(depth & 0xFFFFFFu) << 27) |
           static_cast<uint64_t>(submission_index & 0x7FFFFFFu);
}

bool RenderQueue::push(uint32_t pass, uint32_t kind, uint32_t texture_plus_one,
                       float depth01, uint16_t entity, uint16_t mesh,
                       uint16_t material)
{
    if (m_count >= kMaxDraws) {
        return false;
    }
    DrawCommand& cmd = m_sorted[m_count];
    cmd.key = make_key(pass, kind, texture_plus_one, depth01, m_count);
    cmd.entity = entity;
    cmd.mesh = mesh;
    cmd.material = material;
    cmd.pad = 0;
    ++m_count;
    return true;
}

void RenderQueue::sort()
{
    // LSD radix over 8-bit digits, stable, so the embedded submission index
    // only matters as the final tie-break.
    //
    // This used to use 16-bit digits: four passes over a 65,536-entry
    // histogram, each pass clearing it and prefix-summing it. That is about
    // 786,000 iterations across 256 KB of table NO MATTER how many commands
    // are queued, and the profiler measured it at 16.4 ms of a 33.6 ms frame
    // for a scene with 114 of them (M13 task 3). Two things were wrong with
    // it: the cost did not scale with the work, and a 256 KB table has no
    // hope in an 8 KB data cache, so nearly every touch was a miss on a
    // machine where misses are the expensive thing.
    //
    // 256-entry digits make the fixed cost 8 x 512 instead of 4 x 131,072,
    // and the table fits in cache. The uniform-digit skip below then removes
    // most of the remaining passes outright, because the high bytes of a
    // render key are the same for every command in an ordinary scene.
    if (m_count < 2) {
        return;
    }

    DrawCommand* from = m_sorted;
    DrawCommand* to = m_scratch;
    uint32_t histogram[256];
    uint32_t swaps = 0;

    for (uint32_t shift = 0; shift < 64; shift += 8) {
        for (uint32_t i = 0; i < 256u; ++i) {
            histogram[i] = 0;
        }
        for (uint32_t i = 0; i < m_count; ++i) {
            ++histogram[(from[i].key >> shift) & 0xFFu];
        }
        // If every command shares this digit, the pass would copy the array
        // to the scratch buffer unchanged. Skipping keeps the result
        // identical and costs one comparison.
        const uint32_t first_digit =
            static_cast<uint32_t>((from[0].key >> shift) & 0xFFu);
        if (histogram[first_digit] == m_count) {
            continue;
        }
        uint32_t running = 0;
        for (uint32_t i = 0; i < 256u; ++i) {
            const uint32_t n = histogram[i];
            histogram[i] = running;
            running += n;
        }
        for (uint32_t i = 0; i < m_count; ++i) {
            to[histogram[(from[i].key >> shift) & 0xFFu]++] = from[i];
        }
        DrawCommand* tmp = from;
        from = to;
        to = tmp;
        ++swaps;
    }

    // Skipping passes makes the number of swaps unpredictable, so the result
    // can end up in either buffer. An odd count leaves it in the scratch
    // buffer and it has to come back, or the caller would read the
    // unsorted array.
    if ((swaps & 1u) != 0u) {
        for (uint32_t i = 0; i < m_count; ++i) {
            m_sorted[i] = from[i];
        }
    }
}

} // namespace gfx
} // namespace ps2ur
