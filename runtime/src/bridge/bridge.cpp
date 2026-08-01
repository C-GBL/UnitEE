#include "ps2ur/bridge.h"

#include "ps2ur/log.h"

namespace ps2ur {
namespace bridge {

static bool g_initialized = false;

bool init()
{
    if (g_initialized) {
        return true;
    }
    // TODO(spec missing: section 12): generated boundary functions from the
    // binding generator (12.4) register/link here.
    g_initialized = true;
    log(LogLevel::Debug, "bridge: init (stub)");
    return true;
}

void shutdown()
{
    if (!g_initialized) {
        return;
    }
    g_initialized = false;
    log(LogLevel::Debug, "bridge: shutdown (stub)");
}

bool initialized() { return g_initialized; }

} // namespace bridge
} // namespace ps2ur

// ---- extern "C" boundary (ADR-002) -----------------------------------------

extern "C" void ps2ur_debug_log(const char* utf8)
{
    if (utf8 == nullptr) {
        utf8 = "(null)";
    }
    ps2ur::log(ps2ur::LogLevel::Info, "%s", utf8);
}
