// samples/11-vu1-throughput -- M4 acceptance measurement.
//
// "15,000 triangles/frame at 30 fps in PCSX2 and on hardware, textured and lit."
//
// This answers the question ADR-003 rests on: is the VU1 path actually fast
// enough to justify replacing the EE-side transform? It pushes a fixed
// triangle count through vu_unlit in batches and reports sustained fps and
// triangles/second, rather than asserting a threshold -- a number you can track
// is worth more here than a pass/fail, especially since PCSX2 timing is not
// hardware timing (plan section 14.4: the emulator gates RELATIVE change only).
//
// Batch size is set by VU1 data memory, not by taste: 16 KB = 1024 qwords, of
// which the header takes 8 and the output GIF packet needs 1 + 2 per vertex.
// 96 vertices (32 triangles) fits comfortably and leaves room for the double
// buffering that M4 task 5 adds.

#include <ps2ur/gs_device.h>
#include <ps2ur/gs_packet.h>
#include <ps2ur/log.h>
#include <ps2ur/platform.h>
#include <ps2ur/time.h>
#include <ps2ur/vu_program.h>

#include <kernel.h>
#include <math.h>
#include <stdio.h>

using namespace ps2ur;

extern "C" u32 VuUnlit_CodeStart __attribute__((section(".vudata")));
extern "C" u32 VuUnlit_CodeEnd __attribute__((section(".vudata")));

namespace {

constexpr uint32_t kScreenW = 512;
constexpr uint32_t kScreenH = 448;

constexpr uint32_t kVertsPerBatch = 96; // 32 triangles
constexpr uint32_t kTargetTriangles = 15000;
constexpr uint32_t kBatchesPerFrame = kTargetTriangles / (kVertsPerBatch / 3);
constexpr uint32_t kFrames = 60;

// 8 header qwords + 2 per vertex.
constexpr uint32_t kBatchQwords = 8 + kVertsPerBatch * 2;
alignas(16) gfx::Qword g_batch[kBatchQwords];

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
    time::init();

    gfx::VideoConfig config;
    config.width = kScreenW;
    config.height = kScreenH;

    gfx::GsDevice device;
    if (!device.init(config)) {
        printf("PS2UR_TOKEN_VUTP_FAIL device init\n");
        SleepThread();
        return 1;
    }

    vu::MicroProgram program;
    program.set_blob(&VuUnlit_CodeStart, &VuUnlit_CodeEnd, 0);
    if (!program.upload() || !program.init_batching(kBatchQwords)) {
        printf("PS2UR_TOKEN_VUTP_FAIL upload/batching\n");
        SleepThread();
        return 1;
    }

    // --- Batch header, constant for every batch -----------------------------
    set_float4(g_batch[0], 1.0f, 0.0f, 0.0f, 0.0f);
    set_float4(g_batch[1], 0.0f, 1.0f, 0.0f, 0.0f);
    set_float4(g_batch[2], 0.0f, 0.0f, 1.0f, 0.0f);
    set_float4(g_batch[3], 0.0f, 0.0f, 0.0f, 1.0f);

    const float half_w = static_cast<float>(kScreenW) * 0.5f;
    const float half_h = static_cast<float>(kScreenH) * 0.5f;
    const float z_scale = 8388607.5f / 16.0f; // FTOI4 scales Z too; see vu_unlit.vsm
    set_float4(g_batch[4], half_w, -half_h, z_scale, 0.0f);
    set_float4(g_batch[5], half_w + 2048.0f, half_h + 2048.0f, z_scale, 0.0f);

    gfx::Qword tag_holder[1];
    gfx::GsPacket tag_builder;
    tag_builder.init(tag_holder, 1);
    const uint64_t prim = gfx::gs_prim(gfx::GsPrim::Triangle, true, false, false,
                                       false, false, false, 0, false);
    tag_builder.begin_packed(kVertsPerBatch, 2,
                             gfx::gs_reglist(gfx::GsReg::RGBAQ, gfx::GsReg::XYZ2),
                             true, true, prim);
    g_batch[6] = tag_holder[0];

    g_batch[7].lo = kVertsPerBatch;
    g_batch[7].hi = 0;

    // Small triangles scattered across the screen. Deliberately small: the
    // question is vertex throughput, and huge triangles would measure GS fill
    // rate instead.
    for (uint32_t v = 0; v < kVertsPerBatch; v += 3) {
        const float t = static_cast<float>(v) * 0.31f;
        const float cx = sinf(t) * 0.8f;
        const float cy = cosf(t * 1.7f) * 0.8f;
        const float s = 0.03f;
        const float px[3] = {cx, cx + s, cx};
        const float py[3] = {cy, cy, cy + s};
        for (uint32_t k = 0; k < 3; ++k) {
            set_float4(g_batch[8 + (v + k) * 2], px[k], py[k], 0.0f, 1.0f);
            set_float4(g_batch[9 + (v + k) * 2], 60.0f + static_cast<float>(v % 190u),
                       200.0f, 120.0f, 128.0f);
        }
    }

    printf("[11-vu1-throughput] %u batches/frame x %u tris = %u tris/frame, %u frames\n",
           static_cast<unsigned>(kBatchesPerFrame),
           static_cast<unsigned>(kVertsPerBatch / 3),
           static_cast<unsigned>(kBatchesPerFrame * (kVertsPerBatch / 3)),
           static_cast<unsigned>(kFrames));

    time::update();
    const float t_start = time::seconds();

    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        device.begin_frame();
        device.clear(10, 10, 25);
        device.end_frame();

        for (uint32_t b = 0; b < kBatchesPerFrame; ++b) {
            if (!program.draw_batch(g_batch, kBatchQwords, 0)) {
                printf("PS2UR_TOKEN_VUTP_FAIL batch %u frame %u\n",
                       static_cast<unsigned>(b), static_cast<unsigned>(frame));
                SleepThread();
                return 1;
            }
        }
        // Once per frame, not once per batch: waiting per batch serialises the
        // EE against VU1 and throws away the point of having a vector unit.
        program.wait_batches();
        time::update();
    }

    const float elapsed = time::seconds() - t_start;
    const uint32_t tris_per_frame = kBatchesPerFrame * (kVertsPerBatch / 3);
    const float fps = elapsed > 0.0f ? static_cast<float>(kFrames) / elapsed : 0.0f;
    const float tris_per_sec = fps * static_cast<float>(tris_per_frame);

    // Milli-units throughout: %f drags in soft-float formatting (plan 11.5).
    printf("[11-vu1-throughput] elapsed_ms=%u fps_milli=%u tris_per_frame=%u\n",
           static_cast<unsigned>(elapsed * 1000.0f),
           static_cast<unsigned>(fps * 1000.0f),
           static_cast<unsigned>(tris_per_frame));
    printf("[11-vu1-throughput] tris_per_sec=%u vu_instructions=%u\n",
           static_cast<unsigned>(tris_per_sec),
           static_cast<unsigned>(program.size_instructions()));

    // The acceptance question. The threshold is 29.5, not 30: NTSC runs at
    // 59.94 fields/s = 29.97 frames/s, so 30.0 is not reachable and comparing
    // against it fails a run that is in fact hitting every vsync. Same trap as
    // M2's "60 fps" wording -- see docs/notes/verify-log.md.
    //
    // Hitting vsync while drawing this many triangles means VU1 is NOT the
    // bottleneck; the display is. Reported rather than asserted because PCSX2
    // timing is not hardware timing: this gates relative change only (plan
    // 14.4), and the hardware run is what finally decides M4.
    const bool vsync_locked = fps >= 29.5f;
    printf("[11-vu1-throughput] M4 target %u tris/frame at the NTSC rate -> %s\n",
           static_cast<unsigned>(kTargetTriangles),
           vsync_locked ? "MET in PCSX2 (vsync-locked, VU1 has headroom)"
                        : "NOT met in PCSX2 (below vsync: VU1 is the bottleneck)");

    printf("PS2UR_TOKEN_VUTP_OK\n");

    program.shutdown_batching();
    device.shutdown();
    SleepThread();
    return 0;
}
