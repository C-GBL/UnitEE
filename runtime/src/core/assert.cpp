#include "ps2ur/assert.h"

#include "ps2ur/log.h"

#include <cstdlib>

namespace ps2ur {
namespace detail {

[[noreturn]] void assert_fail(const char* expr, const char* file, int line)
{
    log(LogLevel::Error, "assertion failed: (%s) at %s:%d", expr, file, line);
    // Host: abort into the debugger/test harness. PS2 (newlib): abort() traps;
    // a red-screen + SIO dump handler can replace this once gfx is up.
    std::abort();
}

} // namespace detail
} // namespace ps2ur
