// samples/02-gs-clear -- an isolation harness, not a plan deliverable.
//
// Draws NOTHING except GsDevice::clear(), alternating between two very
// different colours every second. It exists because "the screen looks wrong"
// is not a diagnosis: this separates "the clear works" from "the geometry
// path works", which the spinning cube cannot do on its own.
//
// What you should see: the whole screen flipping between deep blue and dark
// orange once per second, with no other content and no flicker. Anything else
// tells you something specific:
//
//   - solid black                -> the clear is not reaching the GS at all
//   - only part of the screen    -> XYOFFSET/SCISSOR or sprite coordinates
//   - stripes or torn halves     -> interlace field/frame mode
//   - speckle or stray lines     -> packet corruption (cache/DMA overlap)
//   - colours never change       -> the frame loop or buffer flip is stuck

#include <ps2ur/gs_device.h>
#include <ps2ur/log.h>
#include <ps2ur/platform.h>

#include <kernel.h>
#include <stdio.h>

using namespace ps2ur;

int main(void)
{
    platform::init();

    gfx::VideoConfig config;
    config.width = 512;
    config.height = 448;
    config.standard = gfx::VideoStandard::NTSC;

    gfx::GsDevice device;
    if (!device.init(config)) {
        printf("PS2UR_TOKEN_GS_CLEAR_FAIL\n");
        SleepThread();
        return 1;
    }

    // 180 frames ~= 3 seconds at 60 Hz: three full colour changes.
    for (uint32_t frame = 0; frame < 180; ++frame) {
        const bool second_colour = ((frame / 60u) & 1u) != 0u;
        device.begin_frame();
        if (second_colour) {
            device.clear(200, 90, 20); // dark orange
        } else {
            device.clear(20, 40, 160); // deep blue
        }
        device.end_frame();
    }

    // Park on a known final colour so the still image is unambiguous: after
    // 180 frames the last block was blue (frames 120-179).
    printf("[02-gs-clear] 180 frames, final colour should be DEEP BLUE\n");
    printf("PS2UR_TOKEN_GS_CLEAR_OK\n");

    SleepThread();
    return 0;
}
