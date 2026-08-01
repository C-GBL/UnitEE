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

    // Draws text at a screen position, in pixels. Newlines advance a line.
    void draw_text(GsDevice& device, int32_t x, int32_t y, const char* text);

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
    uint8_t m_r = 255, m_g = 255, m_b = 255;
};

} // namespace gfx
} // namespace ps2ur
