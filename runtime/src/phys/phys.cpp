#include "ps2ur/phys.h"

#include "ps2ur/log.h"

namespace ps2ur {
namespace phys {

static bool g_initialized = false;

bool init()
{
    if (g_initialized) {
        return true;
    }
    // TODO(spec missing: section 9): raycast/integrator milestones; collider
    // data (convex-decomposed static meshes) arrives via the exporter.
    g_initialized = true;
    log(LogLevel::Debug, "phys: init (stub)");
    return true;
}

void shutdown()
{
    if (!g_initialized) {
        return;
    }
    g_initialized = false;
    log(LogLevel::Debug, "phys: shutdown (stub)");
}

bool initialized() { return g_initialized; }

} // namespace phys
} // namespace ps2ur
