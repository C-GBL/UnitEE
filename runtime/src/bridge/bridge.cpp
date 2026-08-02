#include "ps2ur/bridge.h"

#include "ps2ur/log.h"

#include "generated_bridge.h"

namespace ps2ur {
namespace bridge {

static bool g_initialized = false;

bool init()
{
    if (g_initialized) {
        return true;
    }
    // The generated table references every boundary symbol: this loop both
    // forces the linker to keep the implementations and proves at startup
    // that none is missing -- a gap faults here with a name, not mid-frame.
    for (int i = 0; i < ps2ur_bridge_table_count; ++i) {
        if (ps2ur_bridge_table[i].fn == nullptr) {
            log(LogLevel::Error, "bridge: symbol '%s' missing",
                ps2ur_bridge_table[i].name);
            return false;
        }
    }
    g_initialized = true;
    log(LogLevel::Debug, "bridge: init, %d boundary functions",
        ps2ur_bridge_table_count);
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
