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
    // LSD radix, 4 passes of 16 bits. Counting sort per pass is stable, so
    // the embedded submission index only matters as the final tie-break.
    DrawCommand* from = m_sorted;
    DrawCommand* to = m_scratch;
    static uint32_t histogram[65536]; // 256 KB, static: not on an 8 KB stack

    for (uint32_t shift = 0; shift < 64; shift += 16) {
        for (uint32_t i = 0; i < 65536u; ++i) {
            histogram[i] = 0;
        }
        for (uint32_t i = 0; i < m_count; ++i) {
            ++histogram[(from[i].key >> shift) & 0xFFFFu];
        }
        uint32_t running = 0;
        for (uint32_t i = 0; i < 65536u; ++i) {
            const uint32_t n = histogram[i];
            histogram[i] = running;
            running += n;
        }
        for (uint32_t i = 0; i < m_count; ++i) {
            to[histogram[(from[i].key >> shift) & 0xFFFFu]++] = from[i];
        }
        DrawCommand* tmp = from;
        from = to;
        to = tmp;
    }
    // 4 passes = even number of swaps: 'from' is m_sorted again.
}

} // namespace gfx
} // namespace ps2ur
