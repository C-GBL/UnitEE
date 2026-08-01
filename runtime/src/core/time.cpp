#include "ps2ur/time.h"

#include "ps2ur/platform.h"

namespace ps2ur {
namespace time {

static uint64_t g_boot = 0;
static uint64_t g_prev = 0;
static uint64_t g_curr = 0;
static bool g_initialized = false;

void init()
{
    g_boot = platform::now_ticks();
    g_prev = g_boot;
    g_curr = g_boot;
    g_initialized = true;
}

void update()
{
    if (!g_initialized) {
        init();
        return;
    }
    g_prev = g_curr;
    g_curr = platform::now_ticks();
}

uint64_t ticks()
{
    return platform::now_ticks() - g_boot;
}

// float, not double: no doubles in runtime code (plan section 3.1). Precision
// of a float second counter is fine for frame timing at 30 fps horizons.
float seconds()
{
    return static_cast<float>(ticks()) / static_cast<float>(platform::ticks_per_second());
}

float delta_seconds()
{
    return static_cast<float>(g_curr - g_prev) /
           static_cast<float>(platform::ticks_per_second());
}

} // namespace time
} // namespace ps2ur
