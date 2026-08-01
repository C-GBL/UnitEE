#include "ps2ur/gs_overlay.h"

#include "ps2ur/gs_font.h"
#include "ps2ur/log.h"

#include <cstdio>

namespace ps2ur {
namespace gfx {

namespace {

// The expanded atlas. PSMCT32 is wasteful for a 1-bit font -- PSMT4 with a
// two-entry CLUT would be 8x smaller -- but the indexed path needs M3's
// palette and swizzle machinery, and 32 KB of VRAM is affordable for the one
// texture that is always resident.
// TODO(M3): move the font to PSMT4 once the swizzler exists.
alignas(16) uint32_t g_atlas[DebugOverlayAtlasPixels];

} // namespace

bool DebugOverlay::init(GsDevice& device)
{
    if (m_initialized) {
        return true;
    }

    // Expand the 1-bit glyphs into RGBA. Set pixels are opaque white so the
    // vertex colour selects the text colour through MODULATE; clear pixels are
    // fully transparent, which alpha test then discards.
    for (uint32_t i = 0; i < kAtlasW * kAtlasH; ++i) {
        g_atlas[i] = 0;
    }
    const uint32_t glyph_count = kFontLastChar - kFontFirstChar + 1u;
    for (uint32_t gi = 0; gi < glyph_count; ++gi) {
        const uint32_t cell_x = (gi % kCols) * kGlyphW;
        const uint32_t cell_y = (gi / kCols) * kGlyphH;
        for (uint32_t row = 0; row < kGlyphH; ++row) {
            const uint8_t bits = kFontGlyphs[gi][row];
            for (uint32_t col = 0; col < kGlyphW; ++col) {
                if ((bits >> (7u - col)) & 1u) {
                    const uint32_t px = cell_x + col;
                    const uint32_t py = cell_y + row;
                    // 0x80 alpha is fully opaque on this hardware.
                    g_atlas[py * kAtlasW + px] = 0x80FFFFFFu;
                }
            }
        }
    }

    m_atlas = device.vram().alloc_buffer(kAtlasW, kAtlasH, PixelFormat::PSMCT32,
                                         "debug-font");
    if (!m_atlas.valid()) {
        log(LogLevel::Error, "overlay: no VRAM for the font atlas");
        return false;
    }
    if (!device.upload_texture(g_atlas, m_atlas, kAtlasW, kAtlasH,
                               PixelFormat::PSMCT32)) {
        log(LogLevel::Error, "overlay: font atlas upload failed");
        return false;
    }

    m_initialized = true;
    return true;
}

void DebugOverlay::shutdown(GsDevice& device)
{
    if (!m_initialized) {
        return;
    }
    device.vram().free(m_atlas);
    m_atlas = VramAlloc{};
    m_initialized = false;
}

void DebugOverlay::draw_text(GsDevice& device, int32_t x, int32_t y, const char* text)
{
    if (!m_initialized || text == nullptr) {
        return;
    }

    // Count drawable glyphs first so the GIF tag's NLOOP is right: the tag
    // has to promise exactly how many vertices follow, and a tag that
    // over-promises leaves the GIF waiting for data that never arrives.
    uint32_t glyphs = 0;
    for (const char* p = text; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c >= kFontFirstChar && c <= kFontLastChar && c != ' ') {
            glyphs++;
        }
    }
    if (glyphs == 0) {
        return;
    }

    device.set_texture(m_atlas, kAtlasW, kAtlasH, PixelFormat::PSMCT32);

    GsPacket& packet = device.packet();

    // Alpha-test the transparent background away rather than alpha-blending
    // it: the font is 1-bit, so a test is exact and costs no blend bandwidth.
    // ATST=6 (GREATER), AREF=0, AFAIL=0 (KEEP nothing -> fragment discarded).
    packet.begin_packed_ad(2);
    packet.add_ad(GsReg::TEST_1, gs_test(true, 6, 0, 0, false, 0, false, 1));
    packet.add_ad(GsReg::PRMODECONT, 1);

    const uint64_t prim = gs_prim(GsPrim::Sprite, false, /*textured=*/true, false,
                                  false, false, /*FST*/ true, 0, false);
    // Two vertices per sprite, each carrying colour, UV and position.
    packet.begin_packed(glyphs * 2u, 3,
                        gs_reglist(GsReg::RGBAQ, GsReg::UV, GsReg::XYZ2),
                        false, true, prim);

    const uint32_t advance = kGlyphW * m_scale;
    const uint32_t line_h = kGlyphH * m_scale;
    int32_t pen_x = x;
    int32_t pen_y = y;

    for (const char* p = text; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c == '\n') {
            pen_x = x;
            pen_y += static_cast<int32_t>(line_h);
            continue;
        }
        if (c < kFontFirstChar || c > kFontLastChar) {
            continue;
        }
        if (c == ' ') {
            pen_x += static_cast<int32_t>(advance);
            continue;
        }

        const uint32_t gi = c - kFontFirstChar;
        const uint32_t u0 = ((gi % kCols) * kGlyphW) << 4;
        const uint32_t v0 = ((gi / kCols) * kGlyphH) << 4;
        const uint32_t u1 = u0 + (kGlyphW << 4);
        const uint32_t v1 = v0 + (kGlyphH << 4);

        packet.add_qword(gs_packed_rgbaq(m_r, m_g, m_b, 0x80));
        packet.add_qword(gs_packed_uv(u0, v0));
        packet.add_qword(gs_packed_xyz(gs_coord(pen_x), gs_coord(pen_y), 0));

        packet.add_qword(gs_packed_rgbaq(m_r, m_g, m_b, 0x80));
        packet.add_qword(gs_packed_uv(u1, v1));
        packet.add_qword(gs_packed_xyz(gs_coord(pen_x + static_cast<int32_t>(advance)),
                                       gs_coord(pen_y + static_cast<int32_t>(line_h)), 0));

        pen_x += static_cast<int32_t>(advance);
    }

    // Leave the alpha test off for whatever draws next.
    packet.begin_packed_ad(1);
    packet.add_ad(GsReg::TEST_1, gs_test(false, 0, 0, 0, false, 0, true, 2));
}

void DebugOverlay::printf_at(GsDevice& device, int32_t x, int32_t y,
                             const char* fmt, ...)
{
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    draw_text(device, x, y, buffer);
}

} // namespace gfx
} // namespace ps2ur
