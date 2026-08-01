#include "ps2ur/scene.h"

#include "ps2ur/log.h"

namespace ps2ur {
namespace scene {

static bool g_initialized = false;

bool init()
{
    if (g_initialized) {
        return true;
    }
    // TODO(spec missing: section 10): entity/transform tables are populated
    // from the p2b container; layout unknown until the format spec lands.
    g_initialized = true;
    log(LogLevel::Debug, "scene: init (stub)");
    return true;
}

void shutdown()
{
    if (!g_initialized) {
        return;
    }
    g_initialized = false;
    log(LogLevel::Debug, "scene: shutdown (stub)");
}

bool initialized() { return g_initialized; }

} // namespace scene
} // namespace ps2ur
