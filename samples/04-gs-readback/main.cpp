// samples/04-gs-readback -- self-checking GS verification (plan section 14.3).
//
// This is the sample that ends the "please look at the screen and tell me what
// you see" loop. It clears to a known colour, reads the framebuffer back out
// of VRAM, and ASSERTS the pixels are what was asked for -- then prints
// per-tile CRC32s so a future golden-image test has something to diff.
//
// It validates readback against the clear specifically because the clear is
// already known-good: if the pixels come back wrong, readback is the suspect,
// not the renderer.

#include <ps2ur/gs_device.h>
#include <ps2ur/log.h>
#include <ps2ur/platform.h>

#include <kernel.h>
#include <stdio.h>

using namespace ps2ur;

namespace {

constexpr uint32_t kWidth = 512;
constexpr uint32_t kHeight = 448;

// Read back a modest window rather than the whole screen: 128x128 is enough to
// prove the path and keeps the transfer (and this sample) quick.
constexpr uint32_t kReadW = 128;
constexpr uint32_t kReadH = 128;

// Known clear colour to assert against.
constexpr uint8_t kR = 20;
constexpr uint8_t kG = 140;
constexpr uint8_t kB = 90;

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
        printf("PS2UR_TOKEN_READBACK_FAIL device init\n");
        SleepThread();
        return 1;
    }

    // A couple of frames so the buffer flip settles into steady state.
    for (int i = 0; i < 3; ++i) {
        device.begin_frame();
        device.clear(kR, kG, kB);
        device.end_frame();
    }

    if (!device.read_framebuffer(g_pixels, 0, 0, kReadW, kReadH)) {
        printf("PS2UR_TOKEN_READBACK_FAIL transfer\n");
        SleepThread();
        return 1;
    }

    // Verify every pixel is the clear colour. Report the first mismatch with
    // its coordinates and value -- "some pixels are wrong" is not actionable.
    uint32_t mismatches = 0;
    uint32_t first_x = 0, first_y = 0;
    uint8_t got_r = 0, got_g = 0, got_b = 0;
    for (uint32_t y = 0; y < kReadH; ++y) {
        for (uint32_t x = 0; x < kReadW; ++x) {
            const uint8_t* p = &g_pixels[(y * kReadW + x) * 4u];
            if (p[0] != kR || p[1] != kG || p[2] != kB) {
                if (mismatches == 0) {
                    first_x = x;
                    first_y = y;
                    got_r = p[0];
                    got_g = p[1];
                    got_b = p[2];
                }
                mismatches++;
            }
        }
    }

    printf("[04-gs-readback] read %ux%u from VRAM, expected RGB(%u,%u,%u)\n",
           static_cast<unsigned>(kReadW), static_cast<unsigned>(kReadH),
           static_cast<unsigned>(kR), static_cast<unsigned>(kG),
           static_cast<unsigned>(kB));

    if (mismatches != 0) {
        printf("[04-gs-readback] %u/%u pixels wrong; first at (%u,%u) = RGB(%u,%u,%u)\n",
               static_cast<unsigned>(mismatches),
               static_cast<unsigned>(kReadW * kReadH),
               static_cast<unsigned>(first_x), static_cast<unsigned>(first_y),
               static_cast<unsigned>(got_r), static_cast<unsigned>(got_g),
               static_cast<unsigned>(got_b));
        printf("PS2UR_TOKEN_READBACK_FAIL pixels\n");
        SleepThread();
        return 1;
    }

    // Tile CRCs: the format a golden-image test checks in (plan section 14.3).
    printf("[04-gs-readback] tile CRC32 grid (32x32 tiles):\n");
    for (uint32_t ty = 0; ty < kReadH / 32u; ++ty) {
        for (uint32_t tx = 0; tx < kReadW / 32u; ++tx) {
            const uint32_t crc =
                gfx::GsDevice::tile_crc32(g_pixels, kReadW, kReadH, tx, ty);
            printf("  tile[%u][%u] = %08X\n", static_cast<unsigned>(ty),
                   static_cast<unsigned>(tx), static_cast<unsigned>(crc));
        }
    }

    printf("PS2UR_TOKEN_READBACK_OK\n");

    device.shutdown();
    SleepThread();
    return 0;
}
