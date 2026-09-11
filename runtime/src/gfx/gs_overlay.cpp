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

void DebugOverlay::draw_text_aligned(GsDevice& device, int32_t x, int32_t y,
                                     int32_t w, int32_t h, uint32_t align_h,
                                     uint32_t align_v, const char* text)
{
    if (!m_initialized || text == nullptr) {
        return;
    }

    // Count lines and measure the block height first. Advance rules must
    // mirror draw_text's: in-range characters advance (space included),
    // everything else is skipped.
    const int32_t advance = static_cast<int32_t>(kGlyphW * m_scale);
    const int32_t line_h = static_cast<int32_t>(kGlyphH * m_scale);
    uint32_t lines = 1;
    for (const char* p = text; *p != '\0'; ++p) {
        if (*p == '\n') {
            ++lines;
        }
    }
    const int32_t total_h = static_cast<int32_t>(lines) * line_h;
    int32_t pen_y = y;
    if (align_v == 1u) {
        pen_y += (h - total_h) / 2;
    } else if (align_v == 2u) {
        pen_y += h - total_h;
    }

    const char* p = text;
    while (*p != '\0') {
        // One line at a time: measure it, place it, hand it to draw_text.
        char line[64];
        uint32_t len = 0;
        int32_t line_w = 0;
        while (*p != '\0' && *p != '\n' && len + 1u < sizeof(line)) {
            const unsigned char c = static_cast<unsigned char>(*p);
            if (c >= kFontFirstChar && c <= kFontLastChar) {
                line_w += advance;
            }
            line[len++] = *p++;
        }
        line[len] = '\0';
        if (*p == '\n') {
            ++p;
        }

        int32_t pen_x = x;
        if (align_h == 1u) {
            pen_x += (w - line_w) / 2;
        } else if (align_h == 2u) {
            pen_x += w - line_w;
        }
        draw_text(device, pen_x, pen_y, line);
        pen_y += line_h;
    }
}

void DebugOverlay::fill_rect(GsDevice& device, int32_t x, int32_t y,
                             int32_t w, int32_t h, uint8_t r, uint8_t g,
                             uint8_t b, uint8_t a)
{
    if (!m_initialized || w <= 0 || h <= 0) {
        return;
    }
    GsPacket& packet = device.packet();
    // One flat untextured sprite; ABE on when translucent (PS2 alpha: 0x80
    // opaque) with the standard blend, depth test off (screen-space UI).
    const bool blend = a < 0x80;
    packet.begin_packed_ad(blend ? 3u : 2u);
    packet.add_ad(GsReg::TEST_1, gs_test(false, 0, 0, 0, false, 0, false, 1));
    if (blend) {
        packet.add_ad(GsReg::ALPHA_1, gs_alpha(0, 1, 0, 1));
    }
    packet.add_ad(GsReg::PRMODECONT, 1);

    const uint64_t prim = gs_prim(GsPrim::Sprite, false, /*textured=*/false,
                                  false, blend, false, false, 0, false);
    packet.begin_packed(2, 2, gs_reglist(GsReg::RGBAQ, GsReg::XYZ2), false,
                        true, prim);
    packet.add_qword(gs_packed_rgbaq(r, g, b, a));
    packet.add_qword(gs_packed_xyz(gs_coord(x), gs_coord(y), 0));
    packet.add_qword(gs_packed_rgbaq(r, g, b, a));
    packet.add_qword(gs_packed_xyz(gs_coord(x + w), gs_coord(y + h), 0));

    // Restore the depth test for whatever draws next.
    packet.begin_packed_ad(1);
    packet.add_ad(GsReg::TEST_1, gs_test(false, 0, 0, 0, false, 0, true, 2));
}

void DebugOverlay::textured_rect(GsDevice& device, int32_t x, int32_t y,
                                 int32_t w, int32_t h, uint32_t tex_w,
                                 uint32_t tex_h, uint8_t r, uint8_t g,
                                 uint8_t b, uint8_t a)
{
    // One textured sprite over whatever texture the CALLER just bound --
    // uGUI Image/RawImage (M12.5 task 5). UV addressing (texel fixed point),
    // full texture stretched to the rect; RGBAQ modulates, so the colour is
    // Unity's tint. Alpha blend is always on: UI art leans on its alpha.
    if (!m_initialized || w <= 0 || h <= 0) {
        return;
    }
    GsPacket& packet = device.packet();
    packet.begin_packed_ad(4);
    packet.add_ad(GsReg::TEST_1, gs_test(false, 0, 0, 0, false, 0, false, 1));
    packet.add_ad(GsReg::ALPHA_1, gs_alpha(0, 1, 0, 1));
    // WMS=WMT=CLAMP: UI art never tiles, and the exact-fit UVs below must
    // not bleed the opposite edge in when filtering.
    packet.add_ad(GsReg::CLAMP_1, 5);
    packet.add_ad(GsReg::PRMODECONT, 1);

    const uint64_t prim = gs_prim(GsPrim::Sprite, false, /*textured=*/true,
                                  false, /*blend=*/true, false, /*FST*/ true,
                                  0, false);
    packet.begin_packed(2, 3, gs_reglist(GsReg::RGBAQ, GsReg::UV, GsReg::XYZ2),
                        false, true, prim);
    packet.add_qword(gs_packed_rgbaq(r, g, b, a));
    packet.add_qword(gs_packed_uv(0, 0));
    packet.add_qword(gs_packed_xyz(gs_coord(x), gs_coord(y), 0));
    packet.add_qword(gs_packed_rgbaq(r, g, b, a));
    packet.add_qword(gs_packed_uv(tex_w << 4, tex_h << 4));
    packet.add_qword(gs_packed_xyz(gs_coord(x + w), gs_coord(y + h), 0));

    // Restore the depth test and REPEAT wrapping for whatever draws next.
    packet.begin_packed_ad(2);
    packet.add_ad(GsReg::CLAMP_1, 0);
    packet.add_ad(GsReg::TEST_1, gs_test(false, 0, 0, 0, false, 0, true, 2));
}

void DebugOverlay::draw_text_font(GsDevice& device, const UIFont& font,
                                  int32_t x, int32_t y, int32_t w, int32_t h,
                                  uint32_t align_h, uint32_t align_v,
                                  uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                                  const char* text)
{
    if (!m_initialized || text == nullptr || font.glyph_count == 0) {
        return;
    }

    // Measure the block: line count now, per-line widths as we go. The
    // measure and the draw MUST share one advance rule (12.4 fixed point,
    // out-of-range characters skipped) or alignment drifts by the
    // difference.
    uint32_t lines = 1;
    for (const char* p = text; *p != '\0'; ++p) {
        if (*p == '\n') {
            ++lines;
        }
    }
    const int32_t line_h = static_cast<int32_t>(font.line_height);
    const int32_t total_h = static_cast<int32_t>(lines) * line_h;
    int32_t line_top = y;
    if (align_v == 1u) {
        line_top += (h - total_h) / 2;
    } else if (align_v == 2u) {
        line_top += h - total_h;
    }

    // Count drawable glyphs for the GIF tag's NLOOP promise.
    uint32_t drawable = 0;
    for (const char* p = text; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        const uint32_t gi = static_cast<uint32_t>(c) - font.first_char;
        if (c != '\n' && gi < font.glyph_count && font.glyphs[gi].w > 0 &&
            font.glyphs[gi].h > 0) {
            ++drawable;
        }
    }
    if (drawable == 0) {
        return;
    }

    GsPacket& packet = device.packet();
    packet.begin_packed_ad(4);
    packet.add_ad(GsReg::TEST_1, gs_test(false, 0, 0, 0, false, 0, false, 1));
    // Additive (Cs*As + Cd) for glow passes, the standard blend otherwise.
    packet.add_ad(GsReg::ALPHA_1,
                  m_additive ? gs_alpha(0, 2, 0, 1) : gs_alpha(0, 1, 0, 1));
    packet.add_ad(GsReg::CLAMP_1, 5); // glyph UVs are exact; never wrap
    packet.add_ad(GsReg::PRMODECONT, 1);

    const uint64_t prim = gs_prim(GsPrim::Sprite, false, /*textured=*/true,
                                  false, /*blend=*/true, false, /*FST*/ true,
                                  0, false);
    packet.begin_packed(drawable * 2u, 3,
                        gs_reglist(GsReg::RGBAQ, GsReg::UV, GsReg::XYZ2),
                        false, true, prim);

    const char* p = text;
    while (*p != '\0') {
        // Measure this line (advance sum, 12.4), then walk it again to
        // emit quads from the aligned pen.
        const char* line_start = p;
        int32_t line_w_q4 = 0;
        for (; *p != '\0' && *p != '\n'; ++p) {
            const uint32_t gi =
                static_cast<uint32_t>(static_cast<unsigned char>(*p)) -
                font.first_char;
            if (gi < font.glyph_count) {
                line_w_q4 += font.glyphs[gi].advance_q4;
            }
        }
        int32_t pen_x = x;
        if (align_h == 1u) {
            pen_x += (w - (line_w_q4 >> 4)) / 2;
        } else if (align_h == 2u) {
            pen_x += w - (line_w_q4 >> 4);
        }
        const int32_t baseline = line_top + static_cast<int32_t>(font.ascent);

        int32_t pen_q4 = pen_x << 4;
        for (const char* q = line_start; q != p; ++q) {
            const uint32_t gi =
                static_cast<uint32_t>(static_cast<unsigned char>(*q)) -
                font.first_char;
            if (gi >= font.glyph_count) {
                continue;
            }
            const UIFontGlyph& glyph = font.glyphs[gi];
            if (glyph.w > 0 && glyph.h > 0) {
                const int32_t gx = (pen_q4 >> 4) + glyph.bearing_x;
                const int32_t gy = baseline - glyph.bearing_y;
                const uint32_t u0 = static_cast<uint32_t>(glyph.u) << 4;
                const uint32_t v0 = static_cast<uint32_t>(glyph.v) << 4;
                packet.add_qword(gs_packed_rgbaq(r, g, b, a));
                packet.add_qword(gs_packed_uv(u0, v0));
                packet.add_qword(
                    gs_packed_xyz(gs_coord(gx), gs_coord(gy), 0));
                packet.add_qword(gs_packed_rgbaq(r, g, b, a));
                packet.add_qword(gs_packed_uv(u0 + (glyph.w << 4),
                                              v0 + (glyph.h << 4)));
                packet.add_qword(gs_packed_xyz(gs_coord(gx + glyph.w),
                                               gs_coord(gy + glyph.h), 0));
            }
            pen_q4 += glyph.advance_q4;
        }
        if (*p == '\n') {
            ++p;
        }
        line_top += line_h;
    }

    // Restore the depth test and REPEAT wrapping for whatever draws next.
    packet.begin_packed_ad(2);
    packet.add_ad(GsReg::CLAMP_1, 0);
    packet.add_ad(GsReg::TEST_1, gs_test(false, 0, 0, 0, false, 0, true, 2));
}

void DebugOverlay::textured_rect_sliced(GsDevice& device, int32_t x, int32_t y,
                                        int32_t w, int32_t h, uint32_t tex_w,
                                        uint32_t tex_h, float border_l,
                                        float border_t, float border_r,
                                        float border_b, uint8_t r, uint8_t g,
                                        uint8_t b, uint8_t a)
{
    if (!m_initialized || w <= 0 || h <= 0 || tex_w == 0 || tex_h == 0) {
        return;
    }

    // Borders in source pixels; clamp each axis pair so they fit both the
    // texture and the destination (Unity shrinks them pairwise too).
    float bl = border_l < 0.0f ? 0.0f : border_l;
    float bt = border_t < 0.0f ? 0.0f : border_t;
    float br = border_r < 0.0f ? 0.0f : border_r;
    float bb = border_b < 0.0f ? 0.0f : border_b;
    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);
    const float tw = static_cast<float>(tex_w);
    const float th = static_cast<float>(tex_h);
    if (bl + br > tw) {
        const float s = tw / (bl + br);
        bl *= s;
        br *= s;
    }
    if (bt + bb > th) {
        const float s = th / (bt + bb);
        bt *= s;
        bb *= s;
    }
    if (bl + br > fw) {
        const float s = fw / (bl + br);
        bl *= s;
        br *= s;
    }
    if (bt + bb > fh) {
        const float s = fh / (bt + bb);
        bt *= s;
        bb *= s;
    }

    // Column and row edges, destination in whole pixels, source in texels.
    const int32_t dx[4] = {x, x + static_cast<int32_t>(bl),
                           x + w - static_cast<int32_t>(br), x + w};
    const int32_t dy[4] = {y, y + static_cast<int32_t>(bt),
                           y + h - static_cast<int32_t>(bb), y + h};
    const float sx[4] = {0.0f, bl, tw - br, tw};
    const float sy[4] = {0.0f, bt, th - bb, th};

    // Count non-degenerate patches first: the GIF tag must promise exactly
    // the vertex count that follows.
    uint32_t patches = 0;
    for (int py = 0; py < 3; ++py) {
        for (int px = 0; px < 3; ++px) {
            if (dx[px + 1] > dx[px] && dy[py + 1] > dy[py]) {
                ++patches;
            }
        }
    }
    if (patches == 0) {
        return;
    }

    GsPacket& packet = device.packet();
    packet.begin_packed_ad(4);
    packet.add_ad(GsReg::TEST_1, gs_test(false, 0, 0, 0, false, 0, false, 1));
    packet.add_ad(GsReg::ALPHA_1, gs_alpha(0, 1, 0, 1));
    packet.add_ad(GsReg::CLAMP_1, 5); // WMS=WMT=CLAMP, like textured_rect
    packet.add_ad(GsReg::PRMODECONT, 1);

    const uint64_t prim = gs_prim(GsPrim::Sprite, false, /*textured=*/true,
                                  false, /*blend=*/true, false, /*FST*/ true,
                                  0, false);
    packet.begin_packed(patches * 2u, 3,
                        gs_reglist(GsReg::RGBAQ, GsReg::UV, GsReg::XYZ2),
                        false, true, prim);
    for (int py = 0; py < 3; ++py) {
        for (int px = 0; px < 3; ++px) {
            if (dx[px + 1] <= dx[px] || dy[py + 1] <= dy[py]) {
                continue;
            }
            const uint32_t u0 = static_cast<uint32_t>(sx[px] * 16.0f);
            const uint32_t v0 = static_cast<uint32_t>(sy[py] * 16.0f);
            const uint32_t u1 = static_cast<uint32_t>(sx[px + 1] * 16.0f);
            const uint32_t v1 = static_cast<uint32_t>(sy[py + 1] * 16.0f);
            packet.add_qword(gs_packed_rgbaq(r, g, b, a));
            packet.add_qword(gs_packed_uv(u0, v0));
            packet.add_qword(
                gs_packed_xyz(gs_coord(dx[px]), gs_coord(dy[py]), 0));
            packet.add_qword(gs_packed_rgbaq(r, g, b, a));
            packet.add_qword(gs_packed_uv(u1, v1));
            packet.add_qword(
                gs_packed_xyz(gs_coord(dx[px + 1]), gs_coord(dy[py + 1]), 0));
        }
    }

    // Restore the depth test and REPEAT wrapping for whatever draws next.
    packet.begin_packed_ad(2);
    packet.add_ad(GsReg::CLAMP_1, 0);
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
