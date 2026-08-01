// PS2 (EE) platform layer -- compiling-but-inert stubs. Only compiled when
// PS2UR_PLATFORM_PS2 is defined (CMake selects this directory for the ps2
// config); the guard below is belt-and-braces.
//
// Real implementation lands once the ps2dev toolchain is installed (env PS2DEV,
// default C:/Users/Ash/ps2dev; PS2SDK = %PS2DEV%/ps2sdk). Plan sections 3.1,
// 3.4, 4.1.
#include "ps2ur/platform.h"

#if defined(PS2UR_PLATFORM_PS2)

#include <loadfile.h>
#include <sifrpc.h>
#include <stdio.h>

// Embedded IOP modules (irx_blobs.S).
extern "C" unsigned char iomanx_irx_start[];
extern "C" unsigned char iomanx_irx_end[];
extern "C" unsigned char filexio_irx_start[];
extern "C" unsigned char filexio_irx_end[];

namespace ps2ur {
namespace platform {

bool init()
{
    // SIF RPC first: file I/O (host:, cdrom0:, mc0:) and every other IOP
    // service rides on it. Discovered the hard way -- fopen("host:...")
    // fails with no useful error when this is missing, because the fio RPC
    // endpoint was never brought up (plan section 3.5).
    SifInitRpc(0);

    // ps2sdk's newlib does file I/O exclusively through fileXio, which is NOT
    // a ROM module: without iomanX + fileXio on the IOP every open() fails
    // instantly with no useful error (found the hard way -- the compile guard
    // in io_common.h forbidding direct fio use is the hint). Load our embedded
    // copies once.
    SifLoadFileInit();
    int mod_ret = 0;
    const int iomanx_id = SifExecModuleBuffer(
        iomanx_irx_start,
        static_cast<u32>(iomanx_irx_end - iomanx_irx_start), 0, nullptr,
        &mod_ret);
    const int filexio_id = SifExecModuleBuffer(
        filexio_irx_start,
        static_cast<u32>(filexio_irx_end - filexio_irx_start), 0, nullptr,
        &mod_ret);
    if (iomanx_id < 0 || filexio_id < 0) {
        printf("[ps2ur] WARNING: IOP module load failed (iomanX=%d fileXio=%d); "
               "file I/O will not work\n", iomanx_id, filexio_id);
    }

    // TODO(spec: section 9 later milestones): exception handlers, DMAC reset,
    // scratchpad reservation.
    return true;
}

void shutdown()
{
    // OBSERVED 2026-07-31 on PCSX2: returning from main() hands control back to
    // the BIOS, which drops the user on the memory-card/disc browser. That is
    // correct behaviour for a bare ELF, but it is NOT what a shipped game
    // should do -- a title either runs its loop forever or explicitly returns
    // to the launcher. The frame loop must therefore never fall off the end of
    // main(); it parks in SleepThread() or reboots via the IOP.
    //
    // TODO(spec missing: section 9): SIF/IOP teardown, then either park the
    // main thread or hand back to the loader (wLaunchELF) deliberately.
}

void log_sink(int level, const char* message)
{
    // ps2sdk routes stdout over the SIF to the host-side console, which both
    // PCSX2 (EE console log) and ps2client capture. That makes this the one
    // diagnostic channel that works identically in the emulator and on real
    // hardware over ps2link, so it is what every on-target log goes through
    // until a dedicated SIO/on-screen sink exists.
    //
    // VERIFIED 2026-07-31: text printed here appears in PCSX2's emulog with
    // EnableEEConsole=true. Note that PCSX2 only surfaces EE console output
    // when that ini flag is set -- an empty log is a config problem, not a
    // silent runtime.
    //
    // `message` arrives ALREADY FORMATTED by ps2ur::log_va, which prepends
    // "[ps2ur:<level>] ". A sink must therefore emit it verbatim and must not
    // add a second tag of its own -- doing so produced the double-prefixed
    // "[ps2ur:DEBUG] [ps2ur:info ] ..." seen on target during bring-up. The
    // `level` argument is for routing decisions only, never re-formatting.
    (void)level; // EE stdout and stderr both reach the same host console.
    fputs(message, stdout);
    fputc('\n', stdout);

    // TODO(spec missing: section 9): add the EE_SIO sink (sio_putsn) for
    // hardware without ps2link, and optionally mirror to scr_printf on screen
    // during early bring-up before the GS device owns the framebuffer.
}

namespace {

// COP0 Count increments once every two CPU cycles, so at half the EE's
// 294.912 MHz core clock (plan section 3.1).
constexpr uint64_t kCountHz = 294912000ull / 2ull;

uint32_t g_last_count = 0;
uint64_t g_count_high = 0; // accumulated wraps, in units of 2^32 ticks

} // namespace

uint64_t now_ticks()
{
    uint32_t count;
    __asm__ __volatile__("mfc0 %0, $9" : "=r"(count));

    // Count is 32 bits and wraps roughly every 29 seconds at 147 MHz, which is
    // well inside a play session. Extend it by detecting the wrap; this is
    // exact provided it is sampled more than once per wrap period, which the
    // per-frame time::update() guarantees.
    if (count < g_last_count) {
        g_count_high += 1ull << 32;
    }
    g_last_count = count;
    return g_count_high + count;
}

uint64_t ticks_per_second()
{
    return kCountHz;
}

} // namespace platform
} // namespace ps2ur

#endif // PS2UR_PLATFORM_PS2
