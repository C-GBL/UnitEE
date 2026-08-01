#include "ps2ur/gfx.h"

#include "ps2ur/log.h"

namespace ps2ur {
namespace gfx {

static bool g_initialized = false;

bool init()
{
    if (g_initialized) {
        return true;
    }
    // TODO(spec missing: section 9): GS device bring-up. Bootstrap path per
    // ADR-003: EE-built packets over PATH3 first; VU1/PATH1 later. Baseline
    // video mode 512x448i NTSC, PSMCT32 double-buffered + PSMZ24 (section 3.3).
    g_initialized = true;
    log(LogLevel::Debug, "gfx: init (stub)");
    return true;
}

void shutdown()
{
    if (!g_initialized) {
        return;
    }
    g_initialized = false;
    log(LogLevel::Debug, "gfx: shutdown (stub)");
}

bool initialized() { return g_initialized; }

} // namespace gfx
} // namespace ps2ur
