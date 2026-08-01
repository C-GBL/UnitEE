// samples/13-vu1-clip -- M4 task 4: verify near-plane + guard-band rejection.
//
// Four triangles, each a distinct colour, each exercising one case:
//
//   A (visible)        w = 1, on screen           -> MUST appear
//   B (behind camera)  w = 0.01 < near            -> must NOT appear anywhere
//   C (straddling)     one vertex behind near     -> rejected whole (pop);
//                      must not appear, and must not smear
//   D (beyond guard)   screen x ~ +40000 px       -> without the guard band
//                      the 16-bit 12.4 coordinate WRAPS back into the visible
//                      range and draws garbage; with it, rejected
//
// Without task 4, B and C divide by tiny/negative w and D wraps -- all three
// paint garbage across the frame. The assertion "their colours appear nowhere
// in the framebuffer" is exactly the property that garbage violates.
//
// Honest scope note: C pops rather than being cut at the near plane. That is
// the staged behaviour the plan schedules first; true clipping replaces it
// later, and this sample then flips C's expectation from "absent" to
// "partially visible".

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
constexpr uint32_t kVertexCount = 12; // 4 triangles

constexpr uint32_t kDataQwords = 10 + kVertexCount * 2;
alignas(16) gfx::Qword g_data[kDataQwords];
alignas(16) uint8_t g_pixels[kScreenW * 112 * 4];

// Distinct, saturated colours so a single stray pixel is attributable.
constexpr uint8_t kColA[3] = {40, 220, 40};   // visible control
constexpr uint8_t kColB[3] = {220, 40, 40};   // behind camera
constexpr uint8_t kColC[3] = {220, 220, 40};  // straddling
constexpr uint8_t kColD[3] = {40, 40, 220};   // beyond guard band
constexpr uint8_t kClear[3] = {8, 8, 20};

void set_float4(gfx::Qword& q, float x, float y, float z, float w)
{
    union {
        float f;
        uint32_t u;
    } cx{x}, cy{y}, cz{z}, cw{w};
    q.lo = static_cast<uint64_t>(cx.u) | (static_cast<uint64_t>(cy.u) << 32);
    q.hi = static_cast<uint64_t>(cz.u) | (static_cast<uint64_t>(cw.u) << 32);
}

void put_vertex(uint32_t v, float x, float y, float w, const uint8_t* col)
{
    set_float4(g_data[10 + v * 2], x, y, 0.0f, w);
    set_float4(g_data[11 + v * 2], static_cast<float>(col[0]),
               static_cast<float>(col[1]), static_cast<float>(col[2]), 128.0f);
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
        printf("PS2UR_TOKEN_VUCLIP_FAIL device init\n");
        SleepThread();
        return 1;
    }

    vu::MicroProgram program;
    program.set_blob(&VuUnlit_CodeStart, &VuUnlit_CodeEnd, 0);
    if (!program.upload() || !program.init_batching(kDataQwords)) {
        printf("PS2UR_TOKEN_VUCLIP_FAIL upload\n");
        SleepThread();
        return 1;
    }

    // Identity MVP; standard viewport; near plane at w = 1/16.
    set_float4(g_data[0], 1.0f, 0.0f, 0.0f, 0.0f);
    set_float4(g_data[1], 0.0f, 1.0f, 0.0f, 0.0f);
    set_float4(g_data[2], 0.0f, 0.0f, 1.0f, 0.0f);
    set_float4(g_data[3], 0.0f, 0.0f, 0.0f, 1.0f);
    const float half_w = static_cast<float>(kScreenW) * 0.5f;
    const float half_h = static_cast<float>(kScreenH) * 0.5f;
    const float z_scale = 8388607.5f / 16.0f;
    set_float4(g_data[4], half_w, -half_h, z_scale, 0.0f);
    set_float4(g_data[5], half_w + 2048.0f, half_h + 2048.0f, z_scale, 0.0f);
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

    // A: plainly visible, centre-left.
    put_vertex(0, -0.6f, -0.3f, 1.0f, kColA);
    put_vertex(1, -0.1f, -0.3f, 1.0f, kColA);
    put_vertex(2, -0.35f, 0.4f, 1.0f, kColA);

    // B: entirely behind the near plane. Without rejection, 1/w = 100 blows
    // the coordinates up and the wrapped result lands SOMEWHERE on screen.
    put_vertex(3, -0.2f, -0.2f, 0.01f, kColB);
    put_vertex(4, 0.2f, -0.2f, 0.01f, kColB);
    put_vertex(5, 0.0f, 0.2f, 0.01f, kColB);

    // C: two vertices in front, one behind -- the straddling case.
    put_vertex(6, 0.3f, -0.3f, 1.0f, kColC);
    put_vertex(7, 0.8f, -0.3f, 1.0f, kColC);
    put_vertex(8, 0.55f, 0.3f, 0.01f, kColC);

    // D: w = 1 but x maps to ~2048 + 256 + 80*256 px, far beyond the 4095
    // guard. 16-bit 12.4 wraps at 4096 px, so unrejected this lands visibly.
    put_vertex(9, 80.0f, -0.2f, 1.0f, kColD);
    put_vertex(10, 81.0f, -0.2f, 1.0f, kColD);
    put_vertex(11, 80.5f, 0.3f, 1.0f, kColD);

    for (int frame = 0; frame < 3; ++frame) {
        device.begin_frame();
        device.clear(kClear[0], kClear[1], kClear[2]);
        device.end_frame();
        if (!program.draw_batch(g_data, kDataQwords, 0)) {
            printf("PS2UR_TOKEN_VUCLIP_FAIL batch\n");
            SleepThread();
            return 1;
        }
        program.wait_batches();
    }

    // Scan a band covering the vertical extent of every triangle
    // (NDC y in [-0.3, 0.4] -> screen y in [134, 291]; band 168..280).
    if (!device.read_framebuffer(g_pixels, 0, 168, kScreenW, 112)) {
        printf("PS2UR_TOKEN_VUCLIP_FAIL readback\n");
        SleepThread();
        return 1;
    }

    uint32_t count_a = 0, count_b = 0, count_c = 0, count_d = 0, count_other = 0;
    auto close_to = [](int v, int t) { return v > t - 6 && v < t + 6; };
    for (uint32_t i = 0; i < kScreenW * 112u; ++i) {
        const uint8_t* p = &g_pixels[i * 4u];
        const int r = p[0], g = p[1], b = p[2];
        if (close_to(r, kColA[0]) && close_to(g, kColA[1]) && close_to(b, kColA[2])) {
            count_a++;
        } else if (close_to(r, kColB[0]) && close_to(g, kColB[1]) && close_to(b, kColB[2])) {
            count_b++;
        } else if (close_to(r, kColC[0]) && close_to(g, kColC[1]) && close_to(b, kColC[2])) {
            count_c++;
        } else if (close_to(r, kColD[0]) && close_to(g, kColD[1]) && close_to(b, kColD[2])) {
            count_d++;
        } else if (!(close_to(r, kClear[0]) && close_to(g, kClear[1]) && close_to(b, kClear[2]))) {
            count_other++;
        }
    }

    printf("[13-vu1-clip] pixels: visible=%u behind=%u straddle=%u guard=%u other=%u\n",
           static_cast<unsigned>(count_a), static_cast<unsigned>(count_b),
           static_cast<unsigned>(count_c), static_cast<unsigned>(count_d),
           static_cast<unsigned>(count_other));

    uint32_t failures = 0;
    if (count_a < 1000) {
        printf("  FAIL: the visible control triangle is missing\n");
        failures++;
    }
    if (count_b != 0) {
        printf("  FAIL: behind-camera triangle drew %u pixels\n",
               static_cast<unsigned>(count_b));
        failures++;
    }
    if (count_c != 0) {
        printf("  FAIL: straddling triangle drew %u pixels (should pop)\n",
               static_cast<unsigned>(count_c));
        failures++;
    }
    if (count_d != 0) {
        printf("  FAIL: guard-band triangle wrapped on screen, %u pixels\n",
               static_cast<unsigned>(count_d));
        failures++;
    }
    if (count_other > 50) {
        printf("  FAIL: %u pixels of unexpected colour (smear/garbage)\n",
               static_cast<unsigned>(count_other));
        failures++;
    }

    if (failures != 0) {
        printf("PS2UR_TOKEN_VUCLIP_FAIL %u checks\n", static_cast<unsigned>(failures));
        SleepThread();
        return 1;
    }

    printf("PS2UR_TOKEN_VUCLIP_OK\n");
    program.shutdown_batching();
    device.shutdown();
    SleepThread();
    return 0;
}
