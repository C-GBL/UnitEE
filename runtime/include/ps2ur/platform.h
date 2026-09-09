// Platform abstraction (plan section 8: src/platform/{host,ps2}).
// host/ implements this with std::chrono + stdio for unit tests (plan section 4.3);
// ps2/ implements it against the EE once the ps2dev toolchain lands.
#pragma once

#include <cstdint>

namespace ps2ur {
namespace platform {

// 'console_boot' selects a console boot: the IOP is reset the way a retail
// title does it, so the modules loaded next land on a known IOP rather than
// on whatever the launcher left resident, and the sbv "load module buffer"
// patch is applied, without which a real console cannot load a module from
// EE memory at all. Both are verified on a console by the boot probe, and
// both turned out to be harmless under PCSX2, host: included (checked
// 2026-09-08). It still defaults OFF, for two reasons: the emulator boot
// every test has passed with stays exactly what it was, and a
// host-filesystem build is the shape a ps2link development loop on hardware
// takes, where resetting the IOP would take ps2link's own modules down with
// it (M13 task 4).
bool init(bool console_boot = false);
void shutdown();

// 'level' is (int)ps2ur::LogLevel. 'message' is a single formatted line, no newline.
void log_sink(int level, const char* message);

// Monotonic tick counter; epoch is arbitrary but fixed for the process lifetime.
uint64_t now_ticks();
uint64_t ticks_per_second();

// Loads an IOP module, trying the usual storage roots before falling back to
// an embedded copy (M10).
//
// SifExecModuleBuffer is NOT reliable for every module: it loads audsrv
// without starting it, leaving the caller blocked on an RPC server that
// never appears (verify-log M10). A file load of the identical .irx works,
// which is also how shipping titles ship IOP modules -- on the disc. This
// helper encodes that order once: host: for development, cdrom0: for a
// disc build, mass: for USB, then the embedded blob.
//
// 'name' is the bare file name, e.g. "freepad.irx". Returns the module id,
// or a negative error.
int load_irx(const char* name, const void* blob, unsigned blob_size);

// Whether "host:" (PCSX2's host filesystem, or ps2link) is tried as a media
// root at all. A disc-only build turns it off at boot so the game reads
// exactly what a console reads, whatever the emulator happens to serve
// (build profile: Host Filesystem -> PS2_GAME_HOST_FS). Default: on.
void set_host_media_enabled(bool enabled);
bool host_media_enabled();

} // namespace platform
} // namespace ps2ur
