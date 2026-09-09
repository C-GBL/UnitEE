// samples/23-boot-probe -- the hardware sign-of-life ladder (M13 task 4).
//
// A console with no serial link has one output device, the TV, and a black
// screen says nothing about WHERE a boot died. This ELF climbs the startup
// sequence one rung at a time and paints the screen at each rung, using the
// cheapest mechanism that could possibly work at that point:
//
//   red      the ELF is executing and the EE can write GS privileged
//            registers. BGCOLOR is set directly, no DMA, no IOP, no SIF.
//   orange   the IOP was reset and SIF RPC came back up.
//   yellow   iomanX and fileXio loaded from EE memory. This rung needs the
//            sbv "load module buffer" patch on real hardware; the emulator
//            does not.
//   magenta  those module loads FAILED (the ids are printed for the log).
//   blue     the GS is initialised and a frame was DRAWN through the GIF
//            DMA path: the first rung that exercises DMA and the cache.
//   green    that frame read back from VRAM as blue: the draw really
//            landed. This is the success state.
//   cyan     the frame drew but the readback disagreed.
//   white    GS init itself failed.
//
// Each rung holds for about three quarters of a second so a person can read
// the ladder as it climbs. Whatever colour the screen stops on is the rung
// that failed, and the rung after it is where to look.
#include <ps2ur/gs_device.h>

#include <graph.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <kernel.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <sifrpc.h>
#include <stdio.h>

using namespace ps2ur;

extern "C" unsigned char iomanx_irx_start[];
extern "C" unsigned char iomanx_irx_end[];
extern "C" unsigned char filexio_irx_start[];
extern "C" unsigned char filexio_irx_end[];

namespace {

alignas(16) uint8_t g_frame[512 * 448 * 4];

// The CRTC is already scanning whatever the launcher left, so a vsync wait
// works before the GS is ever initialised by us.
void hold(int frames)
{
    for (int i = 0; i < frames; ++i) {
        graph_wait_vsync();
    }
}

// Register-only colour: with both read circuits off the GS outputs BGCOLOR
// over the whole raster. Nothing is drawn, nothing is transferred.
void sign(uint8_t r, uint8_t g, uint8_t b, const char* what)
{
    graph_set_bgcolor(r, g, b);
    graph_disable_output();
    printf("[boot-probe] %s\n", what);
}

} // namespace

int main(void)
{
    // Rung 1: red. If this never appears the ELF did not run, or the EE
    // cannot reach the GS at all.
    sign(160, 0, 0, "1 red: ELF running, GS registers reachable");
    hold(45);

    // Rung 2: a clean IOP, the way a retail title starts. This is what makes
    // the same ELF behave identically under a launcher that left its own
    // modules resident and from a cold disc boot.
    SifInitRpc(0);
    while (!SifIopReset("", 0)) {
    }
    while (!SifIopSync()) {
    }
    SifInitRpc(0);
    SifLoadFileInit();
    SifInitIopHeap();
    // Real hardware's ROM loadfile has no "load module from EE buffer" RPC;
    // this patch adds it. PCSX2 accepts buffer loads without it, which is
    // exactly how a boot can pass every emulator test and die here.
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
    sign(160, 80, 0, "2 orange: IOP reset, SIF RPC up, buffer-load patch applied");
    hold(45);

    // Rung 3: the two IOP modules the runtime cannot do file I/O without.
    int mod_ret = 0;
    const int iomanx = SifExecModuleBuffer(
        iomanx_irx_start, static_cast<u32>(iomanx_irx_end - iomanx_irx_start),
        0, nullptr, &mod_ret);
    const int filexio = SifExecModuleBuffer(
        filexio_irx_start,
        static_cast<u32>(filexio_irx_end - filexio_irx_start), 0, nullptr,
        &mod_ret);
    if (iomanx >= 0 && filexio >= 0) {
        sign(160, 160, 0, "3 yellow: iomanX and fileXio loaded from EE memory");
    } else {
        printf("[boot-probe] iomanX=%d fileXio=%d\n", iomanx, filexio);
        sign(160, 0, 160, "3 magenta: IOP module load FAILED");
    }
    hold(45);

    // Rung 4: the GS proper, then one frame through the GIF DMA path.
    gfx::VideoConfig config;
    config.width = 512;
    config.height = 448;
    config.packet_qwords = 4096;
    gfx::GsDevice device;
    if (!device.init(config)) {
        sign(255, 255, 255, "4 white: GS init FAILED");
        printf("PS2UR_TOKEN_BOOTPROBE_FAIL gs\n");
        SleepThread();
        return 1;
    }
    device.show_solid(0, 0, 255);
    printf("[boot-probe] 4 blue: GS initialised, frame drawn over DMA\n");
    hold(45);

    // Rung 5: prove the draw landed by reading VRAM back. Bytes are R G B A.
    const bool landed = device.read_framebuffer(g_frame, 0, 0, 512, 448) &&
                        g_frame[0] == 0 && g_frame[1] == 0 &&
                        g_frame[2] == 0xFF;
    if (landed) {
        device.show_solid(0, 255, 0);
        printf("[boot-probe] 5 green: readback confirms the draw\n");
        printf("PS2UR_TOKEN_BOOTPROBE_OK\n");
    } else {
        device.show_solid(0, 255, 255);
        printf("[boot-probe] 5 cyan: readback %02x %02x %02x did not match\n",
               g_frame[0], g_frame[1], g_frame[2]);
        printf("PS2UR_TOKEN_BOOTPROBE_FAIL readback\n");
    }
    SleepThread();
    return 0;
}
