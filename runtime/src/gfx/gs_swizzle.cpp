#include "ps2ur/gs_swizzle.h"

#include "ps2ur/assert.h"

namespace ps2ur {
namespace gfx {

void clut_csm1_reorder(const uint32_t* src, uint32_t* dest, uint32_t entries)
{
    PS2UR_ASSERT(src != nullptr && dest != nullptr);
    if (entries != 256u) {
        // Only the 256-entry PSMT8 palette has the block shuffle. A 16-entry
        // PSMT4 CLUT is stored linearly.
        for (uint32_t i = 0; i < entries; ++i) {
            dest[i] = src[i];
        }
        return;
    }

    // 256 entries = 8 groups of 32. Within each group, the four 8-entry blocks
    // at positions 1, 2, 5 and 6 are swapped pairwise: 1<->2 and 5<->6.
    for (uint32_t i = 0; i < 256u; ++i) {
        const uint32_t block = (i >> 3) & 0x7u; // 8-entry block within the group
        const uint32_t rest = i & ~0x38u;       // group base + index within block
        uint32_t swapped = block;
        if (block == 1u) {
            swapped = 2u;
        } else if (block == 2u) {
            swapped = 1u;
        } else if (block == 5u) {
            swapped = 6u;
        } else if (block == 6u) {
            swapped = 5u;
        }
        dest[rest | (swapped << 3)] = src[i];
    }
}

} // namespace gfx
} // namespace ps2ur
