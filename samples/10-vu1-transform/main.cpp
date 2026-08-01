// samples/10-vu1-transform -- M4 task 2: verify the VU1 transform maths.
//
// 09-vu1-path1 proved the route; this proves the arithmetic. VU1 now does the
// work draw_triangles_immediate used to do on the EE: matrix transform,
// perspective divide, viewport mapping and GS fixed-point conversion.
//
// The test is chosen so the answer is predictable to the pixel. An identity MVP
// and clip-space input mean a vertex at NDC (-0.5, -0.5) must land at an exactly
// computable screen pixel, so readback can assert both the COLOUR and the
// POSITION of the rectangle. Getting the transform subtly wrong -- a flipped Y,
// a missing 2048 origin bias, an off-by-16 from FTOI4 -- moves the rectangle
// rather than corrupting it, and this catches all three.

#include <ps2ur/gs_device.h>
#include <ps2ur/gs_packet.h>
#include <ps2ur/log.h>
#include <ps2ur/platform.h>
#include <ps2ur/vu_program.h>

#include <kernel.h>
#include <stdio.h>

using namespace ps2ur;

extern "C" u32 VuUnlit_CodeStart __attribute__((section(".vudata")));
extern "C" u32 VuUnlit_CodeEnd __attribute__((section(".vudata")));

namespace {

constexpr uint32_t kScreenW = 512;
constexpr uint32_t kScreenH = 448;

// Two triangles forming a rectangle in NDC.
constexpr float kNdcMin = -0.5f;
constexpr float kNdcMax = 0.5f;
constexpr uint32_t kVertexCount = 6;

constexpr uint8_t kR = 60, kG = 200, kB = 120;
constexpr uint8_t kClearR = 15, kClearG = 15, kClearB = 35;

// Predicted screen extent, computed the same way the microprogram does:
//   screen_x = ndc_x * (W/2) + (W/2)
//   screen_y = ndc_y * -(H/2) + (H/2)      (Y flips)
constexpr int32_t kExpectX0 = 128; // -0.5 * 256 + 256
constexpr int32_t kExpectX1 = 384; //  0.5 * 256 + 256
constexpr int32_t kExpectY0 = 112; //  0.5 * -224 + 224
constexpr int32_t kExpectY1 = 336; // -0.5 * -224 + 224

// VU1 data memory image, assembled on the EE then unpacked in one go.
// 10 header qwords (layout v2, with clip constants at 6) + 2 per vertex.
alignas(16) gfx::Qword g_vu_data[10 + kVertexCount * 2];
alignas(16) uint8_t g_pixels[kScreenW * 128 * 4];

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
        printf("PS2UR_TOKEN_VUXF_FAIL device init\n");
        SleepThread();
        return 1;
    }

    vu::MicroProgram program;
    program.set_blob(&VuUnlit_CodeStart, &VuUnlit_CodeEnd, 0);
    if (!program.upload()) {
        printf("PS2UR_TOKEN_VUXF_FAIL upload\n");
        SleepThread();
        return 1;
    }

    // --- Header -------------------------------------------------------------
    // MVP = identity, so the vertices below are already in clip space and the
    // viewport mapping is what is under test.
    set_float4(g_vu_data[0], 1.0f, 0.0f, 0.0f, 0.0f);
    set_float4(g_vu_data[1], 0.0f, 1.0f, 0.0f, 0.0f);
    set_float4(g_vu_data[2], 0.0f, 0.0f, 1.0f, 0.0f);
    set_float4(g_vu_data[3], 0.0f, 0.0f, 0.0f, 1.0f);

    const float half_w = static_cast<float>(kScreenW) * 0.5f;
    const float half_h = static_cast<float>(kScreenH) * 0.5f;

    // Z maps NDC [-1,1] to the 24-bit GS range. The /16 is not a fudge: the
    // microprogram uses FTOI4 for the whole vector to get X and Y into 12.4
    // fixed point, which multiplies Z by 16 as well, so Z is pre-divided here
    // to compensate. Doing it in the constants costs nothing per vertex.
    const float z_scale = 8388607.5f / 16.0f;

    set_float4(g_vu_data[4], half_w, -half_h, z_scale, 0.0f);
    // The +2048 is the GS screen origin (see kGsOriginX): the GS subtracts
    // XYOFFSET, so it must be added here or everything lands off-screen.
    set_float4(g_vu_data[5], half_w + 2048.0f, half_h + 2048.0f, z_scale, 0.0f);

    // Clip constants: guard band 0..4095 GS pixels, near plane at w = 1/16.
    // The test vertices all have w = 1, comfortably in front of it.
    set_float4(g_vu_data[6], 4095.0f, 4095.0f, 0.0f, 0.0625f);

    // GIF tag: PACKED, one loop per vertex, RGBAQ then XYZ2 -- the order the
    // microprogram stores them in.
    gfx::Qword tag_holder[1];
    gfx::GsPacket tag_builder;
    tag_builder.init(tag_holder, 1);
    const uint64_t prim = gfx::gs_prim(gfx::GsPrim::Triangle, /*gouraud=*/true, false,
                                       false, false, false, false, 0, false);
    tag_builder.begin_packed(kVertexCount, 2,
                             gfx::gs_reglist(gfx::GsReg::RGBAQ, gfx::GsReg::XYZ2),
                             /*eop=*/true, /*set_prim=*/true, prim);
    g_vu_data[7] = tag_holder[0];

    g_vu_data[8].lo = kVertexCount; // read with ilw.x
    g_vu_data[8].hi = 0;

    // --- Vertices -----------------------------------------------------------
    const float xs[kVertexCount] = {kNdcMin, kNdcMax, kNdcMax, kNdcMin, kNdcMax, kNdcMin};
    const float ys[kVertexCount] = {kNdcMin, kNdcMin, kNdcMax, kNdcMin, kNdcMax, kNdcMax};
    for (uint32_t i = 0; i < kVertexCount; ++i) {
        set_float4(g_vu_data[10 + i * 2], xs[i], ys[i], 0.0f, 1.0f);
        // Colour is converted with FTOI0, so these are plain 0..255 floats.
        // 0x80 alpha is opaque on this hardware.
        set_float4(g_vu_data[11 + i * 2], static_cast<float>(kR), static_cast<float>(kG),
                   static_cast<float>(kB), 128.0f);
    }

    for (int frame = 0; frame < 3; ++frame) {
        device.begin_frame();
        device.clear(kClearR, kClearG, kClearB);
        device.end_frame();

        if (!program.unpack_data(g_vu_data, 10 + kVertexCount * 2, 0)) {
            printf("PS2UR_TOKEN_VUXF_FAIL unpack\n");
            SleepThread();
            return 1;
        }
        if (!program.start()) {
            printf("PS2UR_TOKEN_VUXF_FAIL start\n");
            SleepThread();
            return 1;
        }
        vu::wait_idle();
    }

    // Read a horizontal band through the middle of the expected rectangle so
    // both the interior and the left/right edges are covered in one transfer.
    const uint32_t band_y = static_cast<uint32_t>(kExpectY0 + kExpectY1) / 2u - 64u;
    if (!device.read_framebuffer(g_pixels, 0, band_y, kScreenW, 128)) {
        printf("PS2UR_TOKEN_VUXF_FAIL readback\n");
        SleepThread();
        return 1;
    }

    auto sample = [&](uint32_t x, uint32_t y_in_band, uint8_t* out) {
        const uint8_t* p = &g_pixels[(y_in_band * kScreenW + x) * 4u];
        out[0] = p[0];
        out[1] = p[1];
        out[2] = p[2];
    };
    auto is_fill = [&](uint32_t x, uint32_t y) {
        uint8_t c[3];
        sample(x, y, c);
        return c[0] == kR && c[1] == kG && c[2] == kB;
    };
    auto is_clear = [&](uint32_t x, uint32_t y) {
        uint8_t c[3];
        sample(x, y, c);
        return c[0] == kClearR && c[1] == kClearG && c[2] == kClearB;
    };

    const uint32_t mid = 64; // middle row of the band
    uint32_t failures = 0;

    // Interior must be filled.
    if (!is_fill(256, mid)) {
        uint8_t c[3];
        sample(256, mid, c);
        printf("  centre (256) not filled: RGB(%u,%u,%u)\n", c[0], c[1], c[2]);
        failures++;
    }
    // Well outside the rectangle must still be the clear colour.
    if (!is_clear(32, mid)) {
        failures++;
        printf("  x=32 should be clear\n");
    }
    if (!is_clear(480, mid)) {
        failures++;
        printf("  x=480 should be clear\n");
    }

    // Find the actual left and right edges and compare against the prediction.
    uint32_t left = 0, right = 0;
    for (uint32_t x = 0; x < kScreenW; ++x) {
        if (is_fill(x, mid)) {
            left = x;
            break;
        }
    }
    for (uint32_t x = kScreenW; x > 0; --x) {
        if (is_fill(x - 1u, mid)) {
            right = x;
            break;
        }
    }

    printf("[10-vu1-transform] rectangle x span %u..%u (predicted %d..%d)\n",
           static_cast<unsigned>(left), static_cast<unsigned>(right),
           static_cast<int>(kExpectX0), static_cast<int>(kExpectX1));

    // One pixel of slack for fixed-point rounding at the edges.
    const int dl = static_cast<int>(left) - kExpectX0;
    const int dr = static_cast<int>(right) - kExpectX1;
    if (dl < -1 || dl > 1 || dr < -1 || dr > 1) {
        printf("  edges off by (%d, %d) -- transform is wrong, not just imprecise\n",
               dl, dr);
        failures++;
    }

    if (failures != 0) {
        printf("PS2UR_TOKEN_VUXF_FAIL %u checks\n", static_cast<unsigned>(failures));
        SleepThread();
        return 1;
    }

    printf("[10-vu1-transform] VU1 transform verified: %u vertices, %u instructions\n",
           static_cast<unsigned>(kVertexCount),
           static_cast<unsigned>(program.size_instructions()));
    printf("PS2UR_TOKEN_VUXF_OK\n");

    device.shutdown();
    SleepThread();
    return 0;
}
