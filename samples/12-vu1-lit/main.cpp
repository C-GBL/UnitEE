// samples/12-vu1-lit -- M4 task 3: verify the VU1 lighting maths.
//
// Draws three quads whose normals face the single active light at known
// angles, so the resulting colour is arithmetic rather than opinion:
//
//   normal straight at the light   -> N.L = 1     -> full light + ambient
//   normal 60 degrees off          -> N.L = 0.5   -> half light + ambient
//   normal facing away             -> N.L clamped -> ambient only
//
// The third case is the one worth having: without the MAX against zero in the
// microprogram, a surface facing away is lit by the NEGATIVE of the term and
// comes out darker than ambient, which looks like a shading bug rather than a
// clamping one.

#include <ps2ur/gs_device.h>
#include <ps2ur/gs_packet.h>
#include <ps2ur/log.h>
#include <ps2ur/platform.h>
#include <ps2ur/vu_program.h>

#include <kernel.h>
#include <stdio.h>

using namespace ps2ur;

extern "C" u32 VuLit_CodeStart __attribute__((section(".vudata")));
extern "C" u32 VuLit_CodeEnd __attribute__((section(".vudata")));

namespace {

constexpr uint32_t kScreenW = 512;
constexpr uint32_t kScreenH = 448;

constexpr uint32_t kQuadCount = 3;
constexpr uint32_t kVertexCount = kQuadCount * 6;

// 18 header qwords (layout v2, clip constants at 6) + 3 per vertex.
constexpr uint32_t kDataQwords = 18 + kVertexCount * 3;
alignas(16) gfx::Qword g_data[kDataQwords];
alignas(16) uint8_t g_pixels[kScreenW * 64 * 4];

// One white light straight down -Z, plus a modest ambient.
constexpr float kLightR = 200.0f, kLightG = 200.0f, kLightB = 200.0f;
constexpr float kAmbR = 40.0f, kAmbG = 40.0f, kAmbB = 40.0f;
constexpr float kVertexGrey = 1.0f; // vertex colour is unity so light dominates

void set_float4(gfx::Qword& q, float x, float y, float z, float w)
{
    union {
        float f;
        uint32_t u;
    } cx{x}, cy{y}, cz{z}, cw{w};
    q.lo = static_cast<uint64_t>(cx.u) | (static_cast<uint64_t>(cy.u) << 32);
    q.hi = static_cast<uint64_t>(cz.u) | (static_cast<uint64_t>(cw.u) << 32);
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
        printf("PS2UR_TOKEN_VULIT_FAIL device init\n");
        SleepThread();
        return 1;
    }

    vu::MicroProgram program;
    program.set_blob(&VuLit_CodeStart, &VuLit_CodeEnd, 0);
    if (!program.upload() || !program.init_batching(kDataQwords)) {
        printf("PS2UR_TOKEN_VULIT_FAIL upload\n");
        SleepThread();
        return 1;
    }

    // Identity MVP: vertices are already clip-space.
    set_float4(g_data[0], 1.0f, 0.0f, 0.0f, 0.0f);
    set_float4(g_data[1], 0.0f, 1.0f, 0.0f, 0.0f);
    set_float4(g_data[2], 0.0f, 0.0f, 1.0f, 0.0f);
    set_float4(g_data[3], 0.0f, 0.0f, 0.0f, 1.0f);

    const float half_w = static_cast<float>(kScreenW) * 0.5f;
    const float half_h = static_cast<float>(kScreenH) * 0.5f;
    const float z_scale = 8388607.5f / 16.0f;
    set_float4(g_data[4], half_w, -half_h, z_scale, 0.0f);
    set_float4(g_data[5], half_w + 2048.0f, half_h + 2048.0f, z_scale, 0.0f);

    // Clip constants: guard band + near plane. Test vertices sit at w = 1.
    set_float4(g_data[6], 4095.0f, 4095.0f, 0.0f, 0.0625f);

    gfx::Qword tag_holder[1];
    gfx::GsPacket tag_builder;
    tag_builder.init(tag_holder, 1);
    const uint64_t prim = gfx::gs_prim(gfx::GsPrim::Triangle, true, false, false,
                                       false, false, false, 0, false);
    tag_builder.begin_packed(kVertexCount, 2,
                             gfx::gs_reglist(gfx::GsReg::RGBAQ, gfx::GsReg::XYZ2),
                             true, true, prim);
    g_data[7] = tag_holder[0];
    g_data[8].lo = kVertexCount;
    g_data[8].hi = 0;

    // Light directions as columns: light 0 points along +Z (pre-negated on the
    // EE, so a normal of +Z gives N.L = 1). Lights 1-3 are off.
    set_float4(g_data[9], 0.0f, 0.0f, 0.0f, 0.0f);  // x row
    set_float4(g_data[10], 0.0f, 0.0f, 0.0f, 0.0f); // y row
    set_float4(g_data[11], 1.0f, 0.0f, 0.0f, 0.0f); // z row: only light 0
    set_float4(g_data[12], kLightR, 0.0f, 0.0f, 0.0f); // per-light R
    set_float4(g_data[13], kLightG, 0.0f, 0.0f, 0.0f); // per-light G
    set_float4(g_data[14], kLightB, 0.0f, 0.0f, 0.0f); // per-light B
    set_float4(g_data[15], kAmbR, kAmbG, kAmbB, 0.0f);
    set_float4(g_data[16], 255.0f, 255.0f, 255.0f, 128.0f); // clamp ceiling

    // Three quads side by side, each with a different normal.
    const float nz[kQuadCount] = {1.0f, 0.5f, -1.0f}; // N.L = 1, 0.5, clamped to 0
    const float x0[kQuadCount] = {-0.9f, -0.2f, 0.5f};

    uint32_t v = 0;
    for (uint32_t q = 0; q < kQuadCount; ++q) {
        const float xa = x0[q], xb = x0[q] + 0.4f;
        const float ya = -0.3f, yb = 0.3f;
        const float px[6] = {xa, xb, xb, xa, xb, xa};
        const float py[6] = {ya, ya, yb, ya, yb, yb};
        for (uint32_t k = 0; k < 6; ++k, ++v) {
            set_float4(g_data[18 + v * 3], px[k], py[k], 0.0f, 1.0f);
            set_float4(g_data[19 + v * 3], 0.0f, 0.0f, nz[q], 0.0f);
            set_float4(g_data[20 + v * 3], kVertexGrey, kVertexGrey, kVertexGrey, 1.0f);
        }
    }

    for (int frame = 0; frame < 3; ++frame) {
        device.begin_frame();
        device.clear(0, 0, 0);
        device.end_frame();
        if (!program.draw_batch(g_data, kDataQwords, 0)) {
            printf("PS2UR_TOKEN_VULIT_FAIL batch\n");
            SleepThread();
            return 1;
        }
        program.wait_batches();
    }

    // Band through the middle of the quads (NDC y=0 -> screen y=224).
    if (!device.read_framebuffer(g_pixels, 0, 192, kScreenW, 64)) {
        printf("PS2UR_TOKEN_VULIT_FAIL readback\n");
        SleepThread();
        return 1;
    }

    // Expected: light * N.L + ambient, modulated by a unity vertex colour.
    const float expect[kQuadCount] = {
        kLightR * 1.0f + kAmbR,
        kLightR * 0.5f + kAmbR,
        kAmbR, // clamped: ambient only
    };

    uint32_t failures = 0;
    for (uint32_t q = 0; q < kQuadCount; ++q) {
        // Centre of each quad in screen x.
        const float ndc_mid = x0[q] + 0.2f;
        const uint32_t sx = static_cast<uint32_t>(ndc_mid * half_w + half_w);
        const uint8_t* p = &g_pixels[(32u * kScreenW + sx) * 4u];
        const int want = static_cast<int>(expect[q]);
        const int got = static_cast<int>(p[0]);
        const int diff = got - want;
        const bool ok = diff > -8 && diff < 8;
        printf("  quad %u (N.L=%s): got R=%u want R=%d %s\n",
               static_cast<unsigned>(q),
               q == 0 ? "1.0" : (q == 1 ? "0.5" : "clamped"),
               static_cast<unsigned>(got), want, ok ? "OK" : "MISMATCH");
        if (!ok) {
            failures++;
        }
    }

    if (failures != 0) {
        printf("PS2UR_TOKEN_VULIT_FAIL %u quads\n", static_cast<unsigned>(failures));
        SleepThread();
        return 1;
    }

    printf("[12-vu1-lit] 4-light VU1 lighting verified, %u instructions\n",
           static_cast<unsigned>(program.size_instructions()));
    printf("PS2UR_TOKEN_VULIT_OK\n");

    program.shutdown_batching();
    device.shutdown();
    SleepThread();
    return 0;
}
