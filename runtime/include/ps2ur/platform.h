// Platform abstraction (plan section 8: src/platform/{host,ps2}).
// host/ implements this with std::chrono + stdio for unit tests (plan section 4.3);
// ps2/ implements it against the EE once the ps2dev toolchain lands.
#pragma once

#include <cstdint>

namespace ps2ur {
namespace platform {

bool init();
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

} // namespace platform
} // namespace ps2ur
