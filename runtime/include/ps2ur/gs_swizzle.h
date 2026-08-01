// GS CLUT ordering and PS2 alpha range (plan section 3.3, M3 task 1).
//
// IMPORTANT FINDING, verified on target 2026-08-01 (samples/07-swizzle):
//
//   Texture SWIZZLING is NOT needed for the normal upload path. A host->local
//   transfer (BITBLTBUF/TRXPOS/TRXREG/TRXDIR=0, GIF IMAGE mode) takes a RASTER
//   rectangle and the GS's transfer engine writes it into VRAM in the correct
//   block order in hardware, for free. Uploading pre-swizzled data
//   double-swizzles it: a 128x64 PSMT8 test went from 32/8192 texels correct
//   (pre-swizzled) to 8192/8192 (raster).
//
//   Plan section 3.3's "textures must be swizzled offline by the exporter"
//   therefore applies only to a path that writes VRAM directly and bypasses
//   the transfer engine. The exporter should emit RASTER indexed data.
//
//   The manual swizzle routines that were here have been removed rather than
//   left in place: their block/column tables did not match the GS, and
//   known-wrong code that looks usable is a trap. If a direct-VRAM fast path is
//   ever needed, write it then and verify it with samples/07-swizzle.
//
//   CLUT reordering, by contrast, IS required -- see below.
#pragma once

#include "ps2ur/gs_format.h"

#include <cstdint>

namespace ps2ur {
namespace gfx {

// Reorders a 256-entry CSM1 palette into the order the GS expects.
//
// REQUIRED, and confirmed on target: unlike texel data, the palette is read
// positionally and the transfer engine does not fix it up. For 8-bit indexed
// textures in CSM1 mode the 256-entry palette is stored with 32-entry blocks
// reordered -- within each group of 8 blocks, blocks 1<->2 and 5<->6 swap.
// Getting this wrong yields textures with correct SHAPES and scrambled
// COLOURS, which plan section 3.3 flags as a good early test case.
//
// 'src' and 'dest' must not alias. Entries are PSMCT32. The transform is its
// own inverse. A 16-entry PSMT4 palette is stored linearly and passes through.
void clut_csm1_reorder(const uint32_t* src, uint32_t* dest, uint32_t entries = 256);

// PS2 alpha is 0-128, where 0x80 means fully opaque -- NOT 0-255. Every alpha
// channel coming from a PC image format must be rescaled or everything renders
// at half transparency. Plan section 9 M3: "This trips up everyone once."
constexpr uint8_t alpha_to_ps2(uint8_t alpha_0_255)
{
    return static_cast<uint8_t>((static_cast<uint32_t>(alpha_0_255) * 128u + 127u) / 255u);
}

constexpr uint8_t alpha_from_ps2(uint8_t alpha_0_128)
{
    const uint32_t a = alpha_0_128 > 128u ? 128u : alpha_0_128;
    return static_cast<uint8_t>((a * 255u + 64u) / 128u);
}

} // namespace gfx
} // namespace ps2ur
