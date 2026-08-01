// samples/08-texcache -- M3 acceptance on target.
//
// "Cache handles a working set 3x VRAM capacity without visual corruption."
//
// 24 textures of 2 pages each (48 pages) are cycled through a 16-page budget,
// so every frame evicts and re-uploads most of them. Each texture is a single
// distinct colour, so a corrupted or stale binding shows up as a cell drawing
// the wrong colour -- which readback checks, cell by cell, rather than leaving
// it to the eye.

#include <ps2ur/gs_device.h>
#include <ps2ur/gs_texcache.h>
#include <ps2ur/log.h>
#include <ps2ur/platform.h>

#include <kernel.h>
#include <stdio.h>

using namespace ps2ur;

namespace {

constexpr uint32_t kScreenW = 512;
constexpr uint32_t kScreenH = 448;

constexpr uint32_t kTexCount = 24;
constexpr uint32_t kTexSize = 64;   // 64x64 PSMCT32 = 2 pages each
constexpr uint32_t kBudgetPages = 16; // 48 pages of content -> 3x oversubscribed

// Drawn as a 6x4 grid of 32x32 cells.
constexpr uint32_t kCols = 6;
constexpr uint32_t kRows = 4;
constexpr uint32_t kCell = 32;
constexpr int32_t kGridX = 64;
constexpr int32_t kGridY = 64;

alignas(16) uint32_t g_textures[kTexCount][kTexSize * kTexSize];
alignas(16) uint8_t g_pixels[(kCols * kCell) * (kRows * kCell) * 4];

struct Rgb {
    uint8_t r, g, b;
};

// Distinct, well-separated colours so a wrong binding is unambiguous.
Rgb colour_for(uint32_t i)
{
    const uint8_t r = static_cast<uint8_t>(40u + ((i * 37u) % 200u));
    const uint8_t g = static_cast<uint8_t>(40u + ((i * 91u) % 200u));
    const uint8_t b = static_cast<uint8_t>(40u + ((i * 53u) % 200u));
    return Rgb{r, g, b};
}

} // namespace

int main(void)
{
    platform::init();

    for (uint32_t t = 0; t < kTexCount; ++t) {
        const Rgb c = colour_for(t);
        const uint32_t packed = static_cast<uint32_t>(c.r) |
                                (static_cast<uint32_t>(c.g) << 8) |
                                (static_cast<uint32_t>(c.b) << 16) | (0x80u << 24);
        for (uint32_t i = 0; i < kTexSize * kTexSize; ++i) {
            g_textures[t][i] = packed;
        }
    }

    gfx::VideoConfig config;
    config.width = kScreenW;
    config.height = kScreenH;
    // Worst case every texture misses in one frame: 24 x 1024 qwords of image
    // payload. See the sizing warning on TextureCache.
    config.packet_qwords = 32768;

    gfx::GsDevice device;
    if (!device.init(config)) {
        printf("PS2UR_TOKEN_TEXCACHE_FAIL device init\n");
        SleepThread();
        return 1;
    }

    gfx::TextureCache cache;
    cache.init(kBudgetPages);

    gfx::TextureCache::Handle handles[kTexCount];
    for (uint32_t t = 0; t < kTexCount; ++t) {
        handles[t] = cache.add(g_textures[t], kTexSize, kTexSize,
                               gfx::PixelFormat::PSMCT32, nullptr, 0, "cell");
        if (handles[t] == gfx::TextureCache::kInvalidHandle) {
            printf("PS2UR_TOKEN_TEXCACHE_FAIL add %u\n", static_cast<unsigned>(t));
            SleepThread();
            return 1;
        }
    }

    const uint32_t kFrames = 30;
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        device.begin_frame();
        cache.begin_frame();
        device.clear(0, 0, 0);

        for (uint32_t t = 0; t < kTexCount; ++t) {
            if (!cache.bind(device, handles[t])) {
                printf("PS2UR_TOKEN_TEXCACHE_FAIL bind %u frame %u\n",
                       static_cast<unsigned>(t), static_cast<unsigned>(frame));
                SleepThread();
                return 1;
            }

            const int32_t cx = kGridX + static_cast<int32_t>((t % kCols) * kCell);
            const int32_t cy = kGridY + static_cast<int32_t>((t / kCols) * kCell);
            const int32_t x1 = cx + static_cast<int32_t>(kCell);
            const int32_t y1 = cy + static_cast<int32_t>(kCell);
            const uint32_t uv1 = kTexSize << 4;

            gfx::GsDevice::TexVertex quad[6];
            const int32_t xs[6] = {cx, x1, x1, cx, x1, cx};
            const int32_t ys[6] = {cy, cy, y1, cy, y1, y1};
            const uint32_t us[6] = {0, uv1, uv1, 0, uv1, 0};
            const uint32_t vs[6] = {0, 0, uv1, 0, uv1, uv1};
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
            device.draw_textured_triangles(quad, 6);
        }

        device.end_frame();

        if (device.packet().overflowed()) {
            printf("PS2UR_TOKEN_TEXCACHE_FAIL packet overflow frame %u\n",
                   static_cast<unsigned>(frame));
            SleepThread();
            return 1;
        }
    }

    const gfx::TextureCache::Stats& s = cache.stats();
    printf("[08-texcache] %u textures (%u pages) through a %u-page budget, %u frames\n",
           static_cast<unsigned>(kTexCount),
           static_cast<unsigned>(kTexCount * 2u), static_cast<unsigned>(kBudgetPages),
           static_cast<unsigned>(kFrames));
    printf("[08-texcache] hits=%u misses=%u evict/frame=%u peak_uploads/frame=%u failed=%u\n",
           static_cast<unsigned>(s.hits), static_cast<unsigned>(s.misses),
           static_cast<unsigned>(s.evictions_this_frame),
           static_cast<unsigned>(s.peak_uploads_per_frame),
           static_cast<unsigned>(s.failed_binds));

    if (s.resident_pages > kBudgetPages) {
        printf("PS2UR_TOKEN_TEXCACHE_FAIL over budget: %u > %u pages\n",
               static_cast<unsigned>(s.resident_pages),
               static_cast<unsigned>(kBudgetPages));
        SleepThread();
        return 1;
    }

    // Verify the last frame actually rendered the right colour in every cell.
    const uint32_t rw = kCols * kCell;
    const uint32_t rh = kRows * kCell;
    if (!device.read_framebuffer(g_pixels, static_cast<uint32_t>(kGridX),
                                 static_cast<uint32_t>(kGridY), rw, rh)) {
        printf("PS2UR_TOKEN_TEXCACHE_FAIL readback\n");
        SleepThread();
        return 1;
    }

    uint32_t bad = 0;
    for (uint32_t t = 0; t < kTexCount; ++t) {
        // Sample the middle of each cell to stay away from edge filtering.
        const uint32_t sx = (t % kCols) * kCell + kCell / 2u;
        const uint32_t sy = (t / kCols) * kCell + kCell / 2u;
        const uint8_t* p = &g_pixels[(sy * rw + sx) * 4u];
        const Rgb want = colour_for(t);
        const int dr = static_cast<int>(p[0]) - static_cast<int>(want.r);
        const int dg = static_cast<int>(p[1]) - static_cast<int>(want.g);
        const int db = static_cast<int>(p[2]) - static_cast<int>(want.b);
        const bool ok = (dr > -6 && dr < 6) && (dg > -6 && dg < 6) && (db > -6 && db < 6);
        if (!ok) {
            if (bad < 4u) {
                printf("  cell %u got RGB(%u,%u,%u) want RGB(%u,%u,%u)\n",
                       static_cast<unsigned>(t), static_cast<unsigned>(p[0]),
                       static_cast<unsigned>(p[1]), static_cast<unsigned>(p[2]),
                       static_cast<unsigned>(want.r), static_cast<unsigned>(want.g),
                       static_cast<unsigned>(want.b));
            }
            bad++;
        }
    }

    printf("[08-texcache] %u/%u cells correct after thrashing\n",
           static_cast<unsigned>(kTexCount - bad), static_cast<unsigned>(kTexCount));

    if (bad != 0) {
        printf("PS2UR_TOKEN_TEXCACHE_FAIL %u cells\n", static_cast<unsigned>(bad));
        SleepThread();
        return 1;
    }

    printf("PS2UR_TOKEN_TEXCACHE_OK\n");
    cache.shutdown(device);
    device.shutdown();
    SleepThread();
    return 0;
}
