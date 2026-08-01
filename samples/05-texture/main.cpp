// samples/05-texture -- M2 task 6, verified without looking at the screen.
//
// Uploads a 64x64 PSMCT32 checkerboard, draws it as a screen-aligned quad,
// then reads the framebuffer back and asserts the two checker colours landed
// where they should. Because readback is already validated against the clear
// (04-gs-readback), a failure here points at the texture path, not the
// transfer.

#include <ps2ur/gs_device.h>
#include <ps2ur/log.h>
#include <ps2ur/platform.h>

#include <kernel.h>
#include <stdio.h>

using namespace ps2ur;

namespace {

constexpr uint32_t kWidth = 512;
constexpr uint32_t kHeight = 448;

constexpr uint32_t kTexSize = 64;
constexpr uint32_t kCheck = 8; // checker square, in texels

// Two strongly distinct colours so a misread is obvious.
constexpr uint8_t kAR = 240, kAG = 60, kAB = 30;
constexpr uint8_t kBR = 30, kBG = 90, kBB = 240;

alignas(16) uint32_t g_texture[kTexSize * kTexSize];

// The quad is drawn 1:1 with texels at this origin, so screen (x,y) inside it
// maps to texel (x - kQuadX, y - kQuadY).
constexpr int32_t kQuadX = 64;
constexpr int32_t kQuadY = 64;

alignas(16) uint8_t g_pixels[kTexSize * kTexSize * 4];

uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) << 24);
}

} // namespace

int main(void)
{
    platform::init();

    for (uint32_t y = 0; y < kTexSize; ++y) {
        for (uint32_t x = 0; x < kTexSize; ++x) {
            const bool a = (((x / kCheck) + (y / kCheck)) & 1u) == 0u;
            g_texture[y * kTexSize + x] =
                a ? pack_rgba(kAR, kAG, kAB, 0x80) : pack_rgba(kBR, kBG, kBB, 0x80);
        }
    }

    gfx::VideoConfig config;
    config.width = kWidth;
    config.height = kHeight;

    gfx::GsDevice device;
    if (!device.init(config)) {
        printf("PS2UR_TOKEN_TEXTURE_FAIL device init\n");
        SleepThread();
        return 1;
    }

    const gfx::VramAlloc tex = device.vram().alloc_buffer(
        kTexSize, kTexSize, gfx::PixelFormat::PSMCT32, "checker");
    if (!tex.valid()) {
        printf("PS2UR_TOKEN_TEXTURE_FAIL no vram\n");
        SleepThread();
        return 1;
    }
    device.vram().debug_dump();

    // White vertex colour so MODULATE leaves the texel unchanged -- otherwise
    // the assertions below would have to account for the modulation.
    gfx::GsDevice::TexVertex quad[6];
    const int32_t x0 = kQuadX, y0 = kQuadY;
    const int32_t x1 = kQuadX + static_cast<int32_t>(kTexSize);
    const int32_t y1 = kQuadY + static_cast<int32_t>(kTexSize);
    const uint32_t uv0 = 0;
    const uint32_t uv1 = kTexSize << 4; // 12.4 fixed point

    const int32_t xs[6] = {x0, x1, x1, x0, x1, x0};
    const int32_t ys[6] = {y0, y0, y1, y0, y1, y1};
    const uint32_t us[6] = {uv0, uv1, uv1, uv0, uv1, uv0};
    const uint32_t vs[6] = {uv0, uv0, uv1, uv0, uv1, uv1};
    for (int i = 0; i < 6; ++i) {
        quad[i].x = xs[i];
        quad[i].y = ys[i];
        quad[i].z = 1000;
        quad[i].u = us[i];
        quad[i].v = vs[i];
        quad[i].r = 128; // 128 is unity for MODULATE on this hardware
        quad[i].g = 128;
        quad[i].b = 128;
        quad[i].a = 0x80;
    }

    for (int frame = 0; frame < 3; ++frame) {
        device.begin_frame();
        device.clear(0, 0, 0);
        if (frame == 0) {
            if (!device.upload_texture(g_texture, tex, kTexSize, kTexSize,
                                       gfx::PixelFormat::PSMCT32)) {
                printf("PS2UR_TOKEN_TEXTURE_FAIL upload\n");
                SleepThread();
                return 1;
            }
        }
        device.set_texture(tex, kTexSize, kTexSize, gfx::PixelFormat::PSMCT32);
        device.draw_textured_triangles(quad, 6);
        device.end_frame();
    }

    if (!device.read_framebuffer(g_pixels, static_cast<uint32_t>(kQuadX),
                                 static_cast<uint32_t>(kQuadY), kTexSize, kTexSize)) {
        printf("PS2UR_TOKEN_TEXTURE_FAIL readback\n");
        SleepThread();
        return 1;
    }

    // Sample the middle of four checker squares that should alternate.
    struct Probe {
        uint32_t x, y;
        bool expect_a;
    };
    const Probe probes[4] = {
        {kCheck / 2u, kCheck / 2u, true},
        {kCheck + kCheck / 2u, kCheck / 2u, false},
        {kCheck / 2u, kCheck + kCheck / 2u, false},
        {kCheck + kCheck / 2u, kCheck + kCheck / 2u, true},
    };

    uint32_t bad = 0;
    for (int i = 0; i < 4; ++i) {
        const uint8_t* p = &g_pixels[(probes[i].y * kTexSize + probes[i].x) * 4u];
        const uint8_t er = probes[i].expect_a ? kAR : kBR;
        const uint8_t eg = probes[i].expect_a ? kAG : kBG;
        const uint8_t eb = probes[i].expect_a ? kAB : kBB;
        // Allow a little slack: MODULATE and 8-bit rounding are not exact.
        const int dr = static_cast<int>(p[0]) - static_cast<int>(er);
        const int dg = static_cast<int>(p[1]) - static_cast<int>(eg);
        const int db = static_cast<int>(p[2]) - static_cast<int>(eb);
        const bool ok = (dr > -12 && dr < 12) && (dg > -12 && dg < 12) &&
                        (db > -12 && db < 12);
        printf("  probe(%u,%u) got RGB(%u,%u,%u) want RGB(%u,%u,%u) %s\n",
               static_cast<unsigned>(probes[i].x), static_cast<unsigned>(probes[i].y),
               static_cast<unsigned>(p[0]), static_cast<unsigned>(p[1]),
               static_cast<unsigned>(p[2]), static_cast<unsigned>(er),
               static_cast<unsigned>(eg), static_cast<unsigned>(eb),
               ok ? "OK" : "MISMATCH");
        if (!ok) {
            bad++;
        }
    }

    if (bad != 0) {
        printf("PS2UR_TOKEN_TEXTURE_FAIL %u/4 probes wrong\n", static_cast<unsigned>(bad));
        SleepThread();
        return 1;
    }

    printf("[05-texture] 64x64 PSMCT32 checkerboard uploaded, drawn, verified\n");
    printf("PS2UR_TOKEN_TEXTURE_OK\n");

    device.shutdown();
    SleepThread();
    return 0;
}
