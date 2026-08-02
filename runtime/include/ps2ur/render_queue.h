// Render queue (plan section 9, M8 task 4): draws sorted by a packed 64-bit
// key with a stable LSD radix sort. Opaque draws sort front-to-back for
// early-Z; transparent draws sort back-to-front with Z-write off, after all
// opaques.
//
// Key layout, most significant first:
//   [63:62] pass       0 = opaque, 1 = transparent
//   [61:59] kind       material kind (section 7.3): batches by microprogram
//   [58:51] texture    texture index + 1 (0 = untextured): batches TEX0 sets
//   [50:27] depth      24-bit quantized view depth; opaque ascending,
//                      transparent inverted so bigger depth sorts first
//   [26:0]  entity     tie-break: stable submission order per bucket
#pragma once

#include <cstdint>

namespace ps2ur {
namespace gfx {

struct DrawCommand {
    uint64_t key;
    uint16_t entity;
    uint16_t mesh;
    uint16_t material;
    uint16_t pad;
};

class RenderQueue {
public:
    static constexpr uint32_t kMaxDraws = 1024;

    void clear() { m_count = 0; }
    uint32_t count() const { return m_count; }
    const DrawCommand& command(uint32_t i) const { return m_sorted[i]; }

    // depth01 is view depth normalized to [0,1] (near..far), clamped here.
    // Returns false when full -- the caller decides whether that is fatal.
    bool push(uint32_t pass, uint32_t kind, uint32_t texture_plus_one,
              float depth01, uint16_t entity, uint16_t mesh, uint16_t material);

    // Stable LSD radix sort, 16 bits per pass over the four key quarters.
    void sort();

    static uint64_t make_key(uint32_t pass, uint32_t kind,
                             uint32_t texture_plus_one, float depth01,
                             uint32_t submission_index);

private:
    DrawCommand m_sorted[kMaxDraws];
    DrawCommand m_scratch[kMaxDraws];
    uint32_t m_count = 0;
};

} // namespace gfx
} // namespace ps2ur
