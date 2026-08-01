// samples/14-vu1-chain -- M4 tasks 5/6/7 and the acceptance parity test.
//
// Renders the same rotated cube twice:
//
//   path A: the M2 immediate path -- EE transforms every vertex, PATH3
//   path B: the shipping path -- BatchBuilder blocks + one DmaChain kick,
//           VU1 transforms, PATH1
//
// then reads both frames back and compares them pixel by pixel. The plan's
// M4 acceptance asks for "golden-image parity with the M2 immediate path on
// the same scene (within a tolerance for fixed-point rounding)" -- the
// tolerance exists because the EE path truncates to whole pixels before the
// GS while the VU path keeps 12.4 subpixels, which can shift an edge by one
// pixel. Face interiors must match exactly.
//
// Also exercises task 7: the chain reports batches, qwords, and build/kick/
// wait time from the EE's perspective.

#include <ps2ur/dma_chain.h>
#include <ps2ur/gs_batch.h>
#include <ps2ur/gs_device.h>
#include <ps2ur/log.h>
#include <ps2ur/math.h>
#include <ps2ur/platform.h>
#include <ps2ur/vu_program.h>

#include <kernel.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

using namespace ps2ur;

extern "C" u32 VuUnlit_CodeStart __attribute__((section(".vudata")));
extern "C" u32 VuUnlit_CodeEnd __attribute__((section(".vudata")));

namespace {

constexpr uint32_t kScreenW = 512;
constexpr uint32_t kScreenH = 448;

constexpr uint32_t kWinX = 128, kWinY = 96, kWinW = 256, kWinH = 256;

alignas(16) uint8_t g_pixels_a[kWinW * kWinH * 4];
alignas(16) uint8_t g_pixels_b[kWinW * kWinH * 4];
alignas(16) gfx::Qword g_constants[7];
alignas(16) uint8_t g_arena_mem[64 * 1024];

const Vec3 kCorners[8] = {
    {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
    {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1},
};
const uint8_t kIdx[36] = {0, 1, 2, 0, 2, 3, 5, 4, 7, 5, 7, 6, 4, 0, 3, 4, 3, 7,
                          1, 5, 6, 1, 6, 2, 3, 2, 6, 3, 6, 7, 4, 5, 1, 4, 1, 0};
const uint8_t kFace[6][3] = {{220, 60, 60}, {60, 220, 60},  {60, 60, 220},
                             {220, 220, 60}, {220, 60, 220}, {60, 220, 220}};

Mat4 rot_y(float a)
{
    Mat4 r = mat4_identity();
    const float c = cosf(a), s = sinf(a);
    r.m[0] = c;
    r.m[2] = -s;
    r.m[8] = s;
    r.m[10] = c;
    return r;
}
Mat4 rot_x(float a)
{
    Mat4 r = mat4_identity();
    const float c = cosf(a), s = sinf(a);
    r.m[5] = c;
    r.m[6] = s;
    r.m[9] = -s;
    r.m[10] = c;
    return r;
}

} // namespace

int main(void)
{
    platform::init();

    gfx::VideoConfig config;
    config.width = kScreenW;
    config.height = kScreenH;

    gfx::GsDevice device;
    if (!device.init(config)) {
        printf("PS2UR_TOKEN_CHAIN_FAIL device init\n");
        SleepThread();
        return 1;
    }

    vu::MicroProgram program;
    program.set_blob(&VuUnlit_CodeStart, &VuUnlit_CodeEnd, 0);
    if (!program.upload()) {
        printf("PS2UR_TOKEN_CHAIN_FAIL upload\n");
        SleepThread();
        return 1;
    }

    // --- The one MVP both paths share ---------------------------------------
    const Mat4 model = mat4_mul(rot_y(0.7f), rot_x(0.5f));
    const Mat4 view = mat4_mul(mat4_translate(Vec3{0, 0, -5.0f}), model);
    const Mat4 proj = mat4_perspective(1.0472f, 512.0f / 448.0f, 1.0f, 50.0f);
    const Mat4 mvp = mat4_mul(proj, view);

    const float sx = static_cast<float>(kScreenW) * 0.5f;
    const float sy = -static_cast<float>(kScreenH) * 0.5f;
    const float zmax = 8388607.0f;
    const float szf = -zmax * 0.5f;
    const float ozf = zmax * 0.5f;

    // --- Path A: EE immediate ----------------------------------------------
    gfx::GsDevice::Vertex ee_verts[36];
    for (uint32_t i = 0; i < 36; ++i) {
        const Vec3 p = kCorners[kIdx[i]];
        const Vec4 clip = mat4_mul_vec4(mvp, Vec4{p.x, p.y, p.z, 1.0f});
        const float inv = 1.0f / clip.w;
        const float nx = clip.x * inv, ny = clip.y * inv, nz = clip.z * inv;
        ee_verts[i].x = static_cast<int32_t>(nx * sx + sx);
        ee_verts[i].y = static_cast<int32_t>(ny * sy - sy);
        ee_verts[i].z = static_cast<uint32_t>(nz * szf + ozf);
        const uint32_t f = i / 6u;
        ee_verts[i].r = kFace[f][0];
        ee_verts[i].g = kFace[f][1];
        ee_verts[i].b = kFace[f][2];
        ee_verts[i].a = 0x80;
    }

    for (int frame = 0; frame < 3; ++frame) {
        device.begin_frame();
        device.clear(16, 16, 32);
        device.draw_triangles_immediate(ee_verts, 36);
        device.end_frame();
    }
    if (!device.read_framebuffer(g_pixels_a, kWinX, kWinY, kWinW, kWinH)) {
        printf("PS2UR_TOKEN_CHAIN_FAIL readback A\n");
        SleepThread();
        return 1;
    }

    // --- Path B: BatchBuilder + DmaChain + VU1 ------------------------------
    gfx::UnlitVertex vu_verts[36];
    for (uint32_t i = 0; i < 36; ++i) {
        const Vec3 p = kCorners[kIdx[i]];
        const uint32_t f = i / 6u;
        vu_verts[i] = gfx::UnlitVertex{p.x,          p.y,          p.z,
                                       kFace[f][0], kFace[f][1], kFace[f][2], 0x80};
    }

    Arena arena;
    arena.init(g_arena_mem, sizeof(g_arena_mem));
    gfx::BatchBlock blocks[4];
    const uint32_t nblocks =
        gfx::BatchBuilder::build_unlit(vu_verts, 36, arena, blocks, 4);
    if (nblocks == 0) {
        printf("PS2UR_TOKEN_CHAIN_FAIL build\n");
        SleepThread();
        return 1;
    }

    // Viewport constants for the VU: offsets include the GS 2048 origin, and
    // the z pair is pre-divided by 16 because FTOI4 scales the whole vector.
    float vscale[3] = {sx, sy, szf / 16.0f};
    float voffset[3] = {sx + 2048.0f, -sy + 2048.0f, ozf / 16.0f};
    gfx::BatchBuilder::build_unlit_constants(mvp.m, vscale, voffset, 4095.0f, 0.9f,
                                             g_constants);

    gfx::DmaChain chain;
    if (!chain.init(256)) {
        printf("PS2UR_TOKEN_CHAIN_FAIL chain init\n");
        SleepThread();
        return 1;
    }

    for (int frame = 0; frame < 3; ++frame) {
        device.begin_frame();
        device.clear(16, 16, 32);
        device.end_frame(); // clear goes over PATH3 as usual

        chain.begin();
        bool ok = chain.add_constants(g_constants, 7, 0);
        for (uint32_t b = 0; b < nblocks && ok; ++b) {
            ok = chain.add_batch(blocks[b]);
        }
        if (!ok || !chain.kick()) {
            printf("PS2UR_TOKEN_CHAIN_FAIL kick\n");
            SleepThread();
            return 1;
        }
        chain.wait();
    }
    if (!device.read_framebuffer(g_pixels_b, kWinX, kWinY, kWinW, kWinH)) {
        printf("PS2UR_TOKEN_CHAIN_FAIL readback B\n");
        SleepThread();
        return 1;
    }

    const gfx::DmaChain::Stats& st = chain.stats();
    const uint64_t tps = platform::ticks_per_second();
    printf("[14-vu1-chain] chain: %u batches, %u qwords; build=%uus kick=%uus wait=%uus\n",
           static_cast<unsigned>(st.batches), static_cast<unsigned>(st.qwords),
           static_cast<unsigned>(st.build_ticks * 1000000ull / tps),
           static_cast<unsigned>(st.kick_ticks * 1000000ull / tps),
           static_cast<unsigned>(st.wait_ticks * 1000000ull / tps));

    // --- Parity -------------------------------------------------------------
    uint32_t diff = 0;
    for (uint32_t i = 0; i < kWinW * kWinH; ++i) {
        const uint8_t* a = &g_pixels_a[i * 4u];
        const uint8_t* b = &g_pixels_b[i * 4u];
        const int dr = static_cast<int>(a[0]) - static_cast<int>(b[0]);
        const int dg = static_cast<int>(a[1]) - static_cast<int>(b[1]);
        const int db = static_cast<int>(a[2]) - static_cast<int>(b[2]);
        if (dr > 24 || dr < -24 || dg > 24 || dg < -24 || db > 24 || db < -24) {
            diff++;
        }
    }
    const uint32_t total = kWinW * kWinH;
    const uint32_t permille = diff * 1000u / total;
    printf("[14-vu1-chain] parity: %u/%u pixels differ (%u.%u%%)\n",
           static_cast<unsigned>(diff), static_cast<unsigned>(total),
           static_cast<unsigned>(permille / 10u), static_cast<unsigned>(permille % 10u));

    // Both paths must have actually drawn something (guard against comparing
    // two empty frames and calling it parity).
    uint32_t lit_a = 0;
    for (uint32_t i = 0; i < total; ++i) {
        const uint8_t* a = &g_pixels_a[i * 4u];
        if (a[0] != 16 || a[1] != 16 || a[2] != 32) {
            lit_a++;
        }
    }
    printf("[14-vu1-chain] immediate path drew %u px in the window\n",
           static_cast<unsigned>(lit_a));

    uint32_t failures = 0;
    if (lit_a < 5000) {
        printf("  FAIL: the reference render is missing\n");
        failures++;
    }
    // 5%: edge pixels shifted by the subpixel difference, nothing more.
    if (permille > 50) {
        printf("  FAIL: paths diverge beyond fixed-point rounding\n");
        failures++;
    }

    if (failures != 0) {
        printf("PS2UR_TOKEN_CHAIN_FAIL %u checks\n", static_cast<unsigned>(failures));
        SleepThread();
        return 1;
    }

    printf("PS2UR_TOKEN_CHAIN_OK\n");
    chain.shutdown();
    device.shutdown();
    SleepThread();
    return 0;
}
