// Host (x86-64) platform layer: std::chrono + stdio. Exists so the runtime and
// its unit tests build and run on the workstation (plan sections 4.3, 8).
#include "ps2ur/platform.h"

#include <chrono>
#include <cstdio>

namespace ps2ur {
namespace platform {

using Clock = std::chrono::steady_clock;

static Clock::time_point boot_epoch()
{
    static const Clock::time_point s_epoch = Clock::now();
    return s_epoch;
}

bool init()
{
    boot_epoch(); // latch the epoch
    return true;
}

void shutdown()
{
    std::fflush(stdout);
    std::fflush(stderr);
}

void log_sink(int level, const char* message)
{
    // level >= 2 is Warn/Error (ps2ur::LogLevel); route those to stderr.
    std::FILE* out = (level >= 2) ? stderr : stdout;
    std::fputs(message, out);
    std::fputc('\n', out);
}

uint64_t now_ticks()
{
    const auto elapsed = Clock::now() - boot_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
}

uint64_t ticks_per_second()
{
    return 1000000000ull; // nanoseconds
}

} // namespace platform
} // namespace ps2ur
