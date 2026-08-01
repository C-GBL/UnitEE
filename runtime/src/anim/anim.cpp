#include "ps2ur/anim.h"

#include "ps2ur/log.h"

namespace ps2ur {
namespace anim {

static bool g_initialized = false;

bool init()
{
    if (g_initialized) {
        return true;
    }
    // TODO(spec missing: section 9): clip playback/crossfade milestones;
    // section 10: quantised clip data format from the exporter's anim quantiser.
    g_initialized = true;
    log(LogLevel::Debug, "anim: init (stub)");
    return true;
}

void shutdown()
{
    if (!g_initialized) {
        return;
    }
    g_initialized = false;
    log(LogLevel::Debug, "anim: shutdown (stub)");
}

bool initialized() { return g_initialized; }

} // namespace anim
} // namespace ps2ur
