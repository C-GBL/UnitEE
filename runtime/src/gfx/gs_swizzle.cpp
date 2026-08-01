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

uint32_t mip_level_count(uint32_t w, uint32_t h)
{
    uint32_t levels = 1;
    while (w > 1u || h > 1u) {
        w = w > 1u ? w / 2u : 1u;
        h = h > 1u ? h / 2u : 1u;
        levels++;
    }
    return levels;
}

void mip_downsample_psmct32(const uint32_t* src, uint32_t* dest, uint32_t w, uint32_t h)
{
    PS2UR_ASSERT(src != nullptr && dest != nullptr);
    PS2UR_ASSERT(w >= 2u && h >= 2u && (w & 1u) == 0u && (h & 1u) == 0u);

    const uint32_t dw = w / 2u;
    const uint32_t dh = h / 2u;
    for (uint32_t y = 0; y < dh; ++y) {
        for (uint32_t x = 0; x < dw; ++x) {
            const uint32_t a = src[(y * 2u) * w + (x * 2u)];
            const uint32_t b = src[(y * 2u) * w + (x * 2u + 1u)];
            const uint32_t c = src[(y * 2u + 1u) * w + (x * 2u)];
            const uint32_t d = src[(y * 2u + 1u) * w + (x * 2u + 1u)];

            uint32_t out = 0;
            for (uint32_t shift = 0; shift < 32u; shift += 8u) {
                const uint32_t sum = ((a >> shift) & 0xFFu) + ((b >> shift) & 0xFFu) +
                                     ((c >> shift) & 0xFFu) + ((d >> shift) & 0xFFu);
                // +2 rounds to nearest rather than truncating, which otherwise
                // darkens every level and makes a mip chain visibly dim.
                out |= (((sum + 2u) / 4u) & 0xFFu) << shift;
            }
            dest[y * dw + x] = out;
        }
    }
}

} // namespace gfx
} // namespace ps2ur
