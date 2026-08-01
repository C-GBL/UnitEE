#include "ps2ur/io.h"

#include "ps2ur/log.h"

namespace ps2ur {
namespace io {

static bool g_initialized = false;

bool init()
{
    if (g_initialized) {
        return true;
    }
    // TODO(spec missing: section 10): p2b container loader -- chunk layout,
    // alignment, and endianness are defined by the missing format spec.
    // CDVD + memcard (mcserv/mcman) access per section 3.5.
    g_initialized = true;
    log(LogLevel::Debug, "io: init (stub)");
    return true;
}

void shutdown()
{
    if (!g_initialized) {
        return;
    }
    g_initialized = false;
    log(LogLevel::Debug, "io: shutdown (stub)");
}

bool initialized() { return g_initialized; }

} // namespace io
} // namespace ps2ur
