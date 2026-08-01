// samples/09-vu1-path1 -- M4 task 1: prove the VU1 route works at all.
//
// Draws a solid quad, but NOT the way samples 01-08 do. Those build a GIF
// packet on the EE and send it over PATH3. This one hands the packet to VU1 and
// lets VU1 XGKICK it to the GS over PATH1 -- the path every shipping draw call
// will eventually take (ADR-003).
//
// It deliberately does no transform. The point is to prove five separate
// pieces of plumbing before a single transformed vertex is attempted:
//
//     EE -> DMA -> VIF1 -> MPG upload -> UNPACK -> MSCAL -> VU1 -> XGKICK -> GS
//
// If this draws the quad in the right place and colour, the route is sound and
// vu_unlit.vsm only has to get the maths right. If it does not, the maths was
// never the problem.
//
// Verified by readback, so "it worked" is an assertion rather than an opinion.

#include <ps2ur/gs_device.h>
#include <ps2ur/gs_packet.h>
#include <ps2ur/log.h>
#include <ps2ur/platform.h>
#include <ps2ur/vu_program.h>

#include <kernel.h>
#include <stdio.h>

using namespace ps2ur;

// The microprogram blob, placed by dvp-as (see runtime/src/vu/vu_passthrough.vsm).
extern "C" u32 VuPassthrough_CodeStart __attribute__((section(".vudata")));
extern "C" u32 VuPassthrough_CodeEnd __attribute__((section(".vudata")));

namespace {

constexpr uint32_t kScreenW = 512;
constexpr uint32_t kScreenH = 448;

// The quad VU1 will draw, and the colour it must come out.
constexpr int32_t kQuadX = 96;
constexpr int32_t kQuadY = 80;
constexpr uint32_t kQuadW = 128;
constexpr uint32_t kQuadH = 96;
constexpr uint8_t kR = 230, kG = 70, kB = 40;

alignas(16) gfx::Qword g_vu_packet[8];
alignas(16) uint8_t g_pixels[kQuadW * kQuadH * 4];

} // namespace

int main(void)
{
    platform::init();

    gfx::VideoConfig config;
    config.width = kScreenW;
    config.height = kScreenH;

    gfx::GsDevice device;
    if (!device.init(config)) {
        printf("PS2UR_TOKEN_VU1_FAIL device init\n");
        SleepThread();
        return 1;
    }

    vu::MicroProgram program;
    program.set_blob(&VuPassthrough_CodeStart, &VuPassthrough_CodeEnd, 0);
    if (!program.upload()) {
        printf("PS2UR_TOKEN_VU1_FAIL upload\n");
        SleepThread();
        return 1;
    }
    printf("[09-vu1-path1] microprogram: %u instructions\n",
           static_cast<unsigned>(program.size_instructions()));

    // Build the GIF packet the VU will kick. This is an ordinary PACKED sprite,
    // identical in content to what the PATH3 path would send -- only the route
    // differs.
    gfx::GsPacket packet;
    packet.init(g_vu_packet, 8);
    const uint64_t prim = gfx::gs_prim(gfx::GsPrim::Sprite, false, false, false,
                                       false, false, false, 0, false);
    packet.begin_packed(1, 3,
                        gfx::gs_reglist(gfx::GsReg::RGBAQ, gfx::GsReg::XYZ2,
                                        gfx::GsReg::XYZ2),
                        /*eop=*/true, /*set_prim=*/true, prim);
    packet.add_qword(gfx::gs_packed_rgbaq(kR, kG, kB, 0x80));
    packet.add_qword(gfx::gs_packed_xyz(gfx::gs_coord(kQuadX), gfx::gs_coord(kQuadY), 0));
    packet.add_qword(gfx::gs_packed_xyz(
        gfx::gs_coord(kQuadX + static_cast<int32_t>(kQuadW)),
        gfx::gs_coord(kQuadY + static_cast<int32_t>(kQuadH)), 0));

    if (packet.overflowed()) {
        printf("PS2UR_TOKEN_VU1_FAIL packet overflow\n");
        SleepThread();
        return 1;
    }

    for (int frame = 0; frame < 3; ++frame) {
        // Clear over PATH3 as usual, so a black screen would mean the clear
        // broke rather than the VU.
        device.begin_frame();
        device.clear(20, 20, 40);
        device.end_frame();

        // Now the VU1 route: put the GIF packet in VU data memory and run the
        // program, which XGKICKs it.
        if (!program.unpack_data(g_vu_packet, packet.size(), 0)) {
            printf("PS2UR_TOKEN_VU1_FAIL unpack\n");
            SleepThread();
            return 1;
        }
        if (!program.start()) {
            printf("PS2UR_TOKEN_VU1_FAIL start\n");
            SleepThread();
            return 1;
        }
        vu::wait_idle();
    }

    if (!device.read_framebuffer(g_pixels, static_cast<uint32_t>(kQuadX),
                                 static_cast<uint32_t>(kQuadY), kQuadW, kQuadH)) {
        printf("PS2UR_TOKEN_VU1_FAIL readback\n");
        SleepThread();
        return 1;
    }

    // Every pixel of the quad must be the colour VU1 kicked.
    uint32_t wrong = 0;
    uint32_t fx = 0, fy = 0, gr = 0, gg = 0, gb = 0;
    for (uint32_t y = 0; y < kQuadH; ++y) {
        for (uint32_t x = 0; x < kQuadW; ++x) {
            const uint8_t* p = &g_pixels[(y * kQuadW + x) * 4u];
            if (p[0] != kR || p[1] != kG || p[2] != kB) {
                if (wrong == 0) {
                    fx = x;
                    fy = y;
                    gr = p[0];
                    gg = p[1];
                    gb = p[2];
                }
                wrong++;
            }
        }
    }

    printf("[09-vu1-path1] PATH1 quad %ux%u: %u/%u pixels correct\n",
           static_cast<unsigned>(kQuadW), static_cast<unsigned>(kQuadH),
           static_cast<unsigned>(kQuadW * kQuadH - wrong),
           static_cast<unsigned>(kQuadW * kQuadH));

    if (wrong != 0) {
        printf("  first wrong at (%u,%u): got RGB(%u,%u,%u) want RGB(%u,%u,%u)\n",
               static_cast<unsigned>(fx), static_cast<unsigned>(fy),
               static_cast<unsigned>(gr), static_cast<unsigned>(gg),
               static_cast<unsigned>(gb), static_cast<unsigned>(kR),
               static_cast<unsigned>(kG), static_cast<unsigned>(kB));
        // The clear colour surviving means the VU never kicked anything;
        // anything else means it kicked something wrong.
        printf("  => %s\n", (gr == 20 && gg == 20 && gb == 40)
                                ? "clear colour intact: VU1 did not draw (MPG/MSCAL/XGKICK)"
                                : "VU1 drew, but wrong: check the GIF packet contents");
        printf("PS2UR_TOKEN_VU1_FAIL %u pixels\n", static_cast<unsigned>(wrong));
        SleepThread();
        return 1;
    }

    printf("PS2UR_TOKEN_VU1_OK\n");
    device.shutdown();
    SleepThread();
    return 0;
}
