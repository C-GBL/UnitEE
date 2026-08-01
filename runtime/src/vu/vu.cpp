#include "ps2ur/vu.h"

#include "ps2ur/log.h"

namespace ps2ur {
namespace vu {

static bool g_initialized = false;

bool init()
{
    if (g_initialized) {
        return true;
    }
    // TODO(spec missing: section 9): microprogram manager -- upload .vsm blobs
    // (assembled by dvp-as, section 4.1) to VU1 micro memory, manage the
    // double-buffered 16 KB data memory (section 3.2). Material-kind program
    // set per section 7.3: vu_unlit, vu_lit, vu_skin, vu_sprite.
    g_initialized = true;
    log(LogLevel::Debug, "vu: init (stub)");
    return true;
}

void shutdown()
{
    if (!g_initialized) {
        return;
    }
    g_initialized = false;
    log(LogLevel::Debug, "vu: shutdown (stub)");
}

bool initialized() { return g_initialized; }

} // namespace vu
} // namespace ps2ur
