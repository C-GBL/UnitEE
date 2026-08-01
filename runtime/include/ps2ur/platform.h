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

} // namespace platform
} // namespace ps2ur
