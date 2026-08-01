// samples/06-overlay -- M2 task 7, verified by readback rather than by eye.
//
// Draws text with DebugOverlay, then reads the framebuffer back and checks the
// glyph actually rasterised: a lit pixel where the font bitmap has a set bit,
// background where it does not. That catches the failures a screenshot hides --
// UV off by a cell, alpha test inverted, atlas row stride wrong.

#include <ps2ur/gs_device.h>
#include <ps2ur/gs_font.h>
#include <ps2ur/gs_overlay.h>
#include <ps2ur/log.h>
#include <ps2ur/platform.h>

#include <kernel.h>
#include <stdio.h>

using namespace ps2ur;

namespace {

constexpr uint32_t kWidth = 512;
constexpr uint32_t kHeight = 448;

// Draw a single 'A' at 4x scale so each font pixel becomes a 4x4 block,
// leaving plenty of margin for sampling its centre.
constexpr int32_t kTextX = 64;
constexpr int32_t kTextY = 64;
constexpr uint32_t kScale = 4;

constexpr uint32_t kReadW = 64;
constexpr uint32_t kReadH = 64;
alignas(16) uint8_t g_pixels[kReadW * kReadH * 4];

} // namespace

int main(void)
{
    platform::init();

    gfx::VideoConfig config;
    config.width = kWidth;
    config.height = kHeight;

    gfx::GsDevice device;
    if (!device.init(config)) {
        printf("PS2UR_TOKEN_OVERLAY_FAIL device init\n");
        SleepThread();
        return 1;
    }

    gfx::DebugOverlay overlay;
    overlay.set_scale(kScale);
    overlay.set_colour(255, 255, 255);

    for (int frame = 0; frame < 3; ++frame) {
        device.begin_frame();
        device.clear(0, 0, 0);
        if (frame == 0 && !overlay.init(device)) {
            printf("PS2UR_TOKEN_OVERLAY_FAIL atlas\n");
            SleepThread();
            return 1;
        }
        overlay.printf_at(device, kTextX, kTextY, "A");
        device.end_frame();
    }

    if (!device.read_framebuffer(g_pixels, static_cast<uint32_t>(kTextX),
                                 static_cast<uint32_t>(kTextY), kReadW, kReadH)) {
        printf("PS2UR_TOKEN_OVERLAY_FAIL readback\n");
        SleepThread();
        return 1;
    }

    // Compare every font pixel of 'A' against the rasterised block centre.
    const uint32_t gi = static_cast<uint32_t>('A') - gfx::kFontFirstChar;
    uint32_t checked = 0, wrong = 0;
    uint32_t lit_expected = 0;
    for (uint32_t row = 0; row < 8u; ++row) {
        const uint8_t bits = gfx::kFontGlyphs[gi][row];
        for (uint32_t col = 0; col < 8u; ++col) {
            const bool expect_lit = ((bits >> (7u - col)) & 1u) != 0u;
            if (expect_lit) {
                lit_expected++;
            }
            const uint32_t sx = col * kScale + kScale / 2u;
            const uint32_t sy = row * kScale + kScale / 2u;
            if (sx >= kReadW || sy >= kReadH) {
                continue;
            }
            const uint8_t* p = &g_pixels[(sy * kReadW + sx) * 4u];
            const bool got_lit = (p[0] > 128 && p[1] > 128 && p[2] > 128);
            checked++;
            if (got_lit != expect_lit) {
                if (wrong < 4u) {
                    printf("  mismatch at glyph(%u,%u): expected %s got RGB(%u,%u,%u)\n",
                           static_cast<unsigned>(col), static_cast<unsigned>(row),
                           expect_lit ? "lit" : "background",
                           static_cast<unsigned>(p[0]), static_cast<unsigned>(p[1]),
                           static_cast<unsigned>(p[2]));
                }
                wrong++;
            }
        }
    }

    printf("[06-overlay] glyph 'A' at %ux scale: %u/%u sample points correct "
           "(%u lit pixels expected)\n",
           static_cast<unsigned>(kScale), static_cast<unsigned>(checked - wrong),
           static_cast<unsigned>(checked), static_cast<unsigned>(lit_expected));

    if (wrong != 0) {
        printf("PS2UR_TOKEN_OVERLAY_FAIL %u wrong\n", static_cast<unsigned>(wrong));
        SleepThread();
        return 1;
    }

    printf("PS2UR_TOKEN_OVERLAY_OK\n");

    overlay.shutdown(device);
    device.shutdown();
    SleepThread();
    return 0;
}
