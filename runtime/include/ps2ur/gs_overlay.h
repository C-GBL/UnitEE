// Debug text overlay (plan section 9, M2 task 7).
//
// "A bitmap font blitter with Printf -- you will use this constantly."
//
// The font lives in a small pinned VRAM texture uploaded once at init. Text is
// drawn as one textured sprite per glyph, batched into a single GIF packet, so
// a whole line of stats costs one state change rather than one per character.
//
// This is a debug facility, not the UI system: plan section 7.1's uGUI subset
// is a separate thing built on the same primitives much later.
#pragma once

#include "ps2ur/gs_device.h"
#include "ps2ur/gs_format.h"

#include <cstdarg>
#include <cstdint>

namespace ps2ur {
namespace gfx {

// Atlas is 128x64 PSMCT32: 96 glyphs in a 16x6 grid of 8x8 cells, padded to a
// power-of-two height because TEX0 requires it.
inline constexpr uint32_t DebugOverlayAtlasPixels = 128 * 64;

// A baked Unity font (M12.5): one (font, size) pair rasterised by Unity's
// own font engine at export, its glyphs packed into an ordinary TEX
// section (white pixels, coverage in the CLUT alpha) and described here.
// Text drawn with it is proportional, antialiased, and at 1:1 baked
// pixels -- exactly what the Editor shows at that size.
struct UIFontGlyph {
    uint16_t u = 0, v = 0;   // texel origin in the atlas
    uint8_t w = 0, h = 0;    // glyph pixels
    int8_t bearing_x = 0;    // pen -> glyph left
    int8_t bearing_y = 0;    // baseline UP to glyph top
    uint16_t advance_q4 = 0; // pen advance, 12.4 fixed point
};

struct UIFont {
    uint32_t texture = 0xFFFFFFFFu; // TEX index of the atlas
    float ascent = 0;               // px, baseline below the line top
    float line_height = 0;          // px per line
    uint32_t first_char = 32;
    uint32_t glyph_count = 0;
    UIFontGlyph glyphs[96];
};

class DebugOverlay {
public:
    // Reserves and uploads the font atlas. Call once, after GsDevice::init().
    // The upload is appended to the current frame packet, so this must sit
    // between begin_frame() and end_frame() like any other upload.
    bool init(GsDevice& device);
    void shutdown(GsDevice& device);

    bool initialized() const { return m_initialized; }

    void set_colour(uint8_t r, uint8_t g, uint8_t b)
    {
        m_r = r;
        m_g = g;
        m_b = b;
    }

    // Scale is an integer multiplier: 1 gives 8x8 glyphs, 2 gives 16x16.
    // Non-integer scaling on a nearest-filtered 8x8 font just looks broken.
    void set_scale(uint32_t scale) { m_scale = scale < 1u ? 1u : scale; }

    // Additive blending (Cs*As + Cd) for the baked-font draws that follow:
    // the ring of passes under a bloomed wordmark stacks towards white
    // instead of each pass painting over the last. Off is the normal blend.
    void set_additive(bool on) { m_additive = on; }

    // Draws text at a screen position, in pixels. Newlines advance a line.
    void draw_text(GsDevice& device, int32_t x, int32_t y, const char* text);

    // Draws text aligned inside a rect (uGUI Text.alignment, M12.5 task 5):
    // align_h/align_v are 0 left/top, 1 centre/middle, 2 right/bottom.
    // Alignment is PER LINE, like Unity's. Measured at draw time so text
    // changed at runtime re-centres, which baking an offset at export
    // could not do. Lines longer than 63 characters are split; UI text is
    // capped at 48 bytes well before that.
    void draw_text_aligned(GsDevice& device, int32_t x, int32_t y, int32_t w,
                           int32_t h, uint32_t align_h, uint32_t align_v,
                           const char* text);

    // Baked-font text (M12.5): proportional glyph run over the atlas the
    // CALLER just bound (like textured_rect, the bind is the caller's).
    // Aligned per line inside the rect; colour modulates the white glyph
    // pixels, alpha rides the CLUT so edges blend, not cut.
    void draw_text_font(GsDevice& device, const UIFont& font, int32_t x,
                        int32_t y, int32_t w, int32_t h, uint32_t align_h,
                        uint32_t align_v, uint8_t r, uint8_t g, uint8_t b,
                        uint8_t a, const char* text);

    // Flat screen-space rectangle (M8 task 8: the uGUI Image primitive).
    // PS2 alpha: a < 0x80 blends, 0x80 is opaque. Depth test is off, like
    // all overlay drawing.
    // A textured sprite over whatever texture the caller bound (uGUI
    // Image/RawImage, M12.5 task 5). Full texture stretched to the rect;
    // colour is the tint, alpha in the PS2 0..0x80 range.
    void textured_rect(GsDevice& device, int32_t x, int32_t y, int32_t w,
                       int32_t h, uint32_t tex_w, uint32_t tex_h, uint8_t r,
                       uint8_t g, uint8_t b, uint8_t a);

    // The 9-slice version (uGUI Image.Type.Sliced): corners keep their
    // authored pixel size, edges stretch along one axis, the centre
    // stretches both. Borders are in SOURCE pixels (Unity sprite.border)
    // and map 1:1 to screen pixels, the same 1:1 the canvas bake uses
    // (deviation 30). Borders that do not fit are scaled down pairwise,
    // which is what Unity does when a sliced image shrinks.
    void textured_rect_sliced(GsDevice& device, int32_t x, int32_t y,
                              int32_t w, int32_t h, uint32_t tex_w,
                              uint32_t tex_h, float border_l, float border_t,
                              float border_r, float border_b, uint8_t r,
                              uint8_t g, uint8_t b, uint8_t a);

    void fill_rect(GsDevice& device, int32_t x, int32_t y, int32_t w,
                   int32_t h, uint8_t r, uint8_t g, uint8_t b,
                   uint8_t a = 0x80);

    // printf-style. Output is truncated at 256 characters -- an overlay line
    // that long is already unreadable, and a fixed buffer keeps this off the
    // heap on a machine with 32 MB.
    void printf_at(GsDevice& device, int32_t x, int32_t y, const char* fmt, ...)
#if defined(__GNUC__)
        __attribute__((format(printf, 5, 6)))
#endif
        ;

    uint32_t glyph_width() const { return kGlyphW * m_scale; }
    uint32_t glyph_height() const { return kGlyphH * m_scale; }

private:
    static constexpr uint32_t kGlyphW = 8;
    static constexpr uint32_t kGlyphH = 8;
    // 96 glyphs in a 16x6 grid of 8x8 cells -> a 128x48 atlas, padded to
    // 128x64 because TEX0 dimensions must be powers of two.
    static constexpr uint32_t kAtlasW = 128;
    static constexpr uint32_t kAtlasH = 64;
    static constexpr uint32_t kCols = kAtlasW / kGlyphW;

    VramAlloc m_atlas;
    bool m_initialized = false;
    uint32_t m_scale = 1;
    bool m_additive = false;
    uint8_t m_r = 255, m_g = 255, m_b = 255;
};

} // namespace gfx
} // namespace ps2ur
