// samples/07-swizzle -- proves indexed (PSMT8) texturing and CSM1 CLUT order
// against real hardware behaviour (plan section 9, M3 task 1).
//
// Uploads an indexed image whose palette index encodes position, plus a
// CSM1-reordered palette, draws it, reads it back, and compares every texel.
//
// This is the experiment that established a correction to plan section 3.3:
// texel data must be uploaded in RASTER order, because the GS transfer engine
// swizzles during a host->local transfer. Pre-swizzling scored 32/8192 texels;
// raster scores 8192/8192. The CLUT, by contrast, IS read positionally and
// must be pre-reordered by the exporter.
//
// The diagnostic below distinguishes the two failure modes: wrong colours that
// still come FROM the palette mean indices are misplaced; colours absent from
// the palette mean the CLUT order is wrong.

#include <ps2ur/gs_device.h>
#include <ps2ur/gs_swizzle.h>
#include <ps2ur/log.h>
#include <ps2ur/platform.h>

#include <kernel.h>
#include <stdio.h>

using namespace ps2ur;

namespace {

constexpr uint32_t kWidth = 512;
constexpr uint32_t kHeight = 448;

// One full PSMT8 page, so the block-order table is exercised completely.
constexpr uint32_t kTexW = 128;
constexpr uint32_t kTexH = 64;

constexpr int32_t kQuadX = 32;
constexpr int32_t kQuadY = 32;

alignas(16) uint8_t g_indices_raster[kTexW * kTexH];
alignas(16) uint32_t g_palette[256];
alignas(16) uint32_t g_palette_csm1[256];
alignas(16) uint8_t g_pixels[kTexW * kTexH * 4];

// Palette entry i is a distinct colour derived from i, so a wrong index or a
// wrong palette slot produces a visibly wrong colour rather than a near miss.
uint32_t palette_colour(uint32_t i)
{
    const uint8_t r = static_cast<uint8_t>((i * 7u) & 0xFFu);
    const uint8_t g = static_cast<uint8_t>((i * 13u) & 0xFFu);
    const uint8_t b = static_cast<uint8_t>((i * 29u) & 0xFFu);
    return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(b) << 16) | (0x80u << 24);
}

} // namespace

int main(void)
{
    platform::init();

    for (uint32_t i = 0; i < 256; ++i) {
        g_palette[i] = palette_colour(i);
    }
    // The GS reads the palette positionally, so the exporter -- here, us --
    // must hand it over pre-shuffled.
    gfx::clut_csm1_reorder(g_palette, g_palette_csm1, 256);

    // Index encodes position, so a misplaced texel is traceable to where it
    // came from rather than just "wrong".
    for (uint32_t y = 0; y < kTexH; ++y) {
        for (uint32_t x = 0; x < kTexW; ++x) {
            g_indices_raster[y * kTexW + x] =
                static_cast<uint8_t>((x * 3u + y * 11u) & 0xFFu);
        }
    }

    gfx::VideoConfig config;
    config.width = kWidth;
    config.height = kHeight;

    gfx::GsDevice device;
    if (!device.init(config)) {
        printf("PS2UR_TOKEN_SWIZZLE_FAIL device init\n");
        SleepThread();
        return 1;
    }

    const gfx::VramAlloc tex =
        device.vram().alloc_buffer(kTexW, kTexH, gfx::PixelFormat::PSMT8, "indexed");
    const gfx::VramAlloc clut =
        device.vram().alloc_buffer(16, 16, gfx::PixelFormat::PSMCT32, "clut");
    if (!tex.valid() || !clut.valid()) {
        printf("PS2UR_TOKEN_SWIZZLE_FAIL no vram\n");
        SleepThread();
        return 1;
    }

    gfx::GsDevice::TexVertex quad[6];
    const int32_t x0 = kQuadX, y0 = kQuadY;
    const int32_t x1 = kQuadX + static_cast<int32_t>(kTexW);
    const int32_t y1 = kQuadY + static_cast<int32_t>(kTexH);
    const int32_t xs[6] = {x0, x1, x1, x0, x1, x0};
    const int32_t ys[6] = {y0, y0, y1, y0, y1, y1};
    const uint32_t us[6] = {0, kTexW << 4, kTexW << 4, 0, kTexW << 4, 0};
    const uint32_t vs[6] = {0, 0, kTexH << 4, 0, kTexH << 4, kTexH << 4};
    for (int i = 0; i < 6; ++i) {
        quad[i].x = xs[i];
        quad[i].y = ys[i];
        quad[i].z = 1000;
        quad[i].u = us[i];
        quad[i].v = vs[i];
        quad[i].r = 128; // unity for MODULATE
        quad[i].g = 128;
        quad[i].b = 128;
        quad[i].a = 0x80;
    }

    for (int frame = 0; frame < 3; ++frame) {
        device.begin_frame();
        device.clear(0, 0, 0);
        if (frame == 0) {
            if (!device.upload_texture(g_indices_raster, tex, kTexW, kTexH,
                                       gfx::PixelFormat::PSMT8) ||
                !device.upload_clut(g_palette_csm1, clut, 256)) {
                printf("PS2UR_TOKEN_SWIZZLE_FAIL upload\n");
                SleepThread();
                return 1;
            }
        }
        device.set_texture_indexed(tex, kTexW, kTexH, gfx::PixelFormat::PSMT8,
                                   clut, 256);
        device.draw_textured_triangles(quad, 6);
        device.end_frame();
    }

    if (!device.read_framebuffer(g_pixels, static_cast<uint32_t>(kQuadX),
                                 static_cast<uint32_t>(kQuadY), kTexW, kTexH)) {
        printf("PS2UR_TOKEN_SWIZZLE_FAIL readback\n");
        SleepThread();
        return 1;
    }

    // Compare every texel. Distinguish "shape wrong" from "colour wrong" by
    // also checking whether the observed colour exists anywhere in the palette.
    uint32_t wrong = 0, wrong_but_in_palette = 0;
    uint32_t first_x = 0, first_y = 0, first_got = 0, first_want = 0;
    for (uint32_t y = 0; y < kTexH; ++y) {
        for (uint32_t x = 0; x < kTexW; ++x) {
            const uint8_t idx = g_indices_raster[y * kTexW + x];
            const uint32_t want = g_palette[idx] & 0x00FFFFFFu;
            const uint8_t* p = &g_pixels[(y * kTexW + x) * 4u];
            const uint32_t got = static_cast<uint32_t>(p[0]) |
                                 (static_cast<uint32_t>(p[1]) << 8) |
                                 (static_cast<uint32_t>(p[2]) << 16);
            if (got != want) {
                if (wrong == 0) {
                    first_x = x;
                    first_y = y;
                    first_got = got;
                    first_want = want;
                }
                for (uint32_t i = 0; i < 256; ++i) {
                    if ((g_palette[i] & 0x00FFFFFFu) == got) {
                        wrong_but_in_palette++;
                        break;
                    }
                }
                wrong++;
            }
        }
    }

    const uint32_t total = kTexW * kTexH;
    printf("[07-swizzle] PSMT8 %ux%u + 256-entry CSM1 CLUT: %u/%u texels correct\n",
           static_cast<unsigned>(kTexW), static_cast<unsigned>(kTexH),
           static_cast<unsigned>(total - wrong), static_cast<unsigned>(total));

    if (wrong != 0) {
        printf("  first mismatch at (%u,%u): got %06X want %06X\n",
               static_cast<unsigned>(first_x), static_cast<unsigned>(first_y),
               static_cast<unsigned>(first_got), static_cast<unsigned>(first_want));
        // If wrong colours are still palette colours, indices are landing in
        // the wrong places -> swizzle order. If they are not in the palette at
        // all, the palette itself is being read wrong -> CLUT order.
        printf("  %u of %u wrong texels show a colour that IS in the palette\n",
               static_cast<unsigned>(wrong_but_in_palette), static_cast<unsigned>(wrong));
        printf("  => %s\n", wrong_but_in_palette > wrong / 2u
                                ? "indices misplaced: suspect the SWIZZLE order"
                                : "colours not from the palette: suspect the CLUT order");
        printf("PS2UR_TOKEN_SWIZZLE_FAIL %u texels\n", static_cast<unsigned>(wrong));
        SleepThread();
        return 1;
    }

    printf("PS2UR_TOKEN_SWIZZLE_OK\n");
    device.shutdown();
    SleepThread();
    return 0;
}
