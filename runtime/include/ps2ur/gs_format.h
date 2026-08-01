// GS pixel formats and VRAM geometry (plan section 3.3).
//
// The GS has 4 MB of embedded VRAM holding *everything*: both colour buffers,
// the Z buffer, and every resident texture. VRAM is organised as 8 KB pages
// made of 256-byte blocks, and how many pixels fit in a page depends entirely
// on the pixel format. Every size calculation in the renderer starts here.
#pragma once

#include <cstdint>

namespace ps2ur {
namespace gfx {

// GS PSM codes. These values are the hardware encodings written into
// FRAME/ZBUF/TEX0/BITBLTBUF registers -- do not renumber them.
enum class PixelFormat : uint8_t {
    PSMCT32  = 0x00, // RGBA8
    PSMCT24  = 0x01, // RGB8, stored in 32-bit slots
    PSMCT16  = 0x02, // RGBA5551
    PSMCT16S = 0x0A, // RGBA5551, "scrambled" page layout
    PSMT8    = 0x13, // 8-bit indexed + CLUT
    PSMT4    = 0x14, // 4-bit indexed + CLUT
    PSMZ32   = 0x30,
    PSMZ24   = 0x31,
    PSMZ16   = 0x32,
    PSMZ16S  = 0x3A,
};

// VRAM structure. Sizes are fixed hardware properties.
inline constexpr uint32_t kVramBytes  = 4u * 1024u * 1024u;
inline constexpr uint32_t kPageBytes  = 8192u;
inline constexpr uint32_t kBlockBytes = 256u;
inline constexpr uint32_t kPageCount  = kVramBytes / kPageBytes;   // 512
inline constexpr uint32_t kBlocksPerPage = kPageBytes / kBlockBytes; // 32

// Pixels covered by one 8 KB page in a given format.
struct PageGeometry {
    uint16_t width;
    uint16_t height;
};

// Every entry satisfies width * height * bits_per_pixel / 8 == 8192.
constexpr PageGeometry page_geometry(PixelFormat fmt)
{
    switch (fmt) {
    case PixelFormat::PSMCT32:
    case PixelFormat::PSMCT24:
    case PixelFormat::PSMZ32:
    case PixelFormat::PSMZ24:
        return PageGeometry{64, 32}; // 2048 px x 4 B
    case PixelFormat::PSMCT16:
    case PixelFormat::PSMCT16S:
    case PixelFormat::PSMZ16:
    case PixelFormat::PSMZ16S:
        return PageGeometry{64, 64}; // 4096 px x 2 B
    case PixelFormat::PSMT8:
        return PageGeometry{128, 64}; // 8192 px x 1 B
    case PixelFormat::PSMT4:
        return PageGeometry{128, 128}; // 16384 px x 0.5 B
    }
    return PageGeometry{64, 32};
}

// Pixels covered by one 256-byte block. Used by the offline swizzler and the
// texture uploader; textures are addressed in blocks, not pages.
constexpr PageGeometry block_geometry(PixelFormat fmt)
{
    switch (fmt) {
    case PixelFormat::PSMCT32:
    case PixelFormat::PSMCT24:
    case PixelFormat::PSMZ32:
    case PixelFormat::PSMZ24:
        return PageGeometry{8, 8};
    case PixelFormat::PSMCT16:
    case PixelFormat::PSMCT16S:
    case PixelFormat::PSMZ16:
    case PixelFormat::PSMZ16S:
        return PageGeometry{16, 8};
    case PixelFormat::PSMT8:
        return PageGeometry{16, 16};
    case PixelFormat::PSMT4:
        return PageGeometry{32, 16};
    }
    return PageGeometry{8, 8};
}

// Bits per pixel. PSMT4 is the reason this is not a byte count.
constexpr uint32_t bits_per_pixel(PixelFormat fmt)
{
    switch (fmt) {
    case PixelFormat::PSMCT32:
    case PixelFormat::PSMZ32:
        return 32;
    case PixelFormat::PSMCT24:
    case PixelFormat::PSMZ24:
        return 32; // 24-bit data occupies a 32-bit slot in VRAM
    case PixelFormat::PSMCT16:
    case PixelFormat::PSMCT16S:
    case PixelFormat::PSMZ16:
    case PixelFormat::PSMZ16S:
        return 16;
    case PixelFormat::PSMT8:
        return 8;
    case PixelFormat::PSMT4:
        return 4;
    }
    return 32;
}

// Integer ceiling divide, used throughout the size maths.
constexpr uint32_t div_round_up(uint32_t a, uint32_t b)
{
    return (a + b - 1u) / b;
}

// Pages needed to hold a width x height buffer. Buffers occupy whole pages in
// both axes: a 512x448 PSMCT32 frame is 8 pages across by 14 down = 112 pages
// = 896 KB, which is exactly the plan's section 15.2 budget line.
constexpr uint32_t pages_for_buffer(uint32_t width, uint32_t height, PixelFormat fmt)
{
    const PageGeometry pg = page_geometry(fmt);
    return div_round_up(width, pg.width) * div_round_up(height, pg.height);
}

// FRAME.FBW / TEX0.TBW are expressed in units of 64 pixels.
constexpr uint32_t buffer_width_units(uint32_t width)
{
    return div_round_up(width, 64u);
}

// Compile-time proof that each page geometry really is 8 KB. If someone adds a
// format with the wrong dimensions, the build fails here rather than producing
// silently corrupt VRAM addresses.
static_assert(page_geometry(PixelFormat::PSMCT32).width *
                  page_geometry(PixelFormat::PSMCT32).height *
                  bits_per_pixel(PixelFormat::PSMCT32) / 8 == kPageBytes,
              "PSMCT32 page geometry must cover exactly 8 KB");
static_assert(page_geometry(PixelFormat::PSMCT16).width *
                  page_geometry(PixelFormat::PSMCT16).height *
                  bits_per_pixel(PixelFormat::PSMCT16) / 8 == kPageBytes,
              "PSMCT16 page geometry must cover exactly 8 KB");
static_assert(page_geometry(PixelFormat::PSMT8).width *
                  page_geometry(PixelFormat::PSMT8).height *
                  bits_per_pixel(PixelFormat::PSMT8) / 8 == kPageBytes,
              "PSMT8 page geometry must cover exactly 8 KB");
static_assert(page_geometry(PixelFormat::PSMT4).width *
                  page_geometry(PixelFormat::PSMT4).height *
                  bits_per_pixel(PixelFormat::PSMT4) / 8 == kPageBytes,
              "PSMT4 page geometry must cover exactly 8 KB");
static_assert(pages_for_buffer(512, 448, PixelFormat::PSMCT32) == 112,
              "512x448 PSMCT32 must be 112 pages (896 KB, plan section 15.2)");

} // namespace gfx
} // namespace ps2ur
