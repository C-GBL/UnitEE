// PS2 (EE) platform layer -- compiling-but-inert stubs. Only compiled when
// PS2UR_PLATFORM_PS2 is defined (CMake selects this directory for the ps2
// config); the guard below is belt-and-braces.
//
// Real implementation lands once the ps2dev toolchain is installed (env PS2DEV,
// default C:/Users/Ash/ps2dev; PS2SDK = %PS2DEV%/ps2sdk). Plan sections 3.1,
// 3.4, 4.1.
#include "ps2ur/platform.h"

#if defined(PS2UR_PLATFORM_PS2)

#include <stdio.h>

namespace ps2ur {
namespace platform {

bool init()
{
    // TODO(ps2dev): EE bring-up -- install exception handlers, reset DMAC,
    // reserve the 16 KB scratchpad at 0x70000000 (section 3.1), SIF init for
    // IOP RPC (section 3.5).
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

uint64_t now_ticks()
{
    // TODO(ps2dev): read COP0 Count (or T0/T1 EE timers) and extend to 64 bits;
    // EE core clock is 294.912 MHz (section 3.1).
    return 0;
}

uint64_t ticks_per_second()
{
    return 294912000ull; // EE clock, section 3.1 -- placeholder until timer choice
}

} // namespace platform
} // namespace ps2ur

#endif // PS2UR_PLATFORM_PS2
