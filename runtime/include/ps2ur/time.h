// Time: ticks/seconds since boot and per-frame delta time (plan section 8: src/core).
// Backed by ps2ur::platform::now_ticks / ticks_per_second.
// float only -- no doubles anywhere in runtime code (plan section 3.1).
#pragma once

#include <cstdint>

namespace ps2ur {
namespace time {

// Latches the boot epoch. Called implicitly by the first update() if omitted.
void init();

// Call exactly once per frame; advances the delta_seconds() window.
void update();

uint64_t ticks();        // platform ticks since init()
float seconds();         // seconds since init()
float delta_seconds();   // seconds between the last two update() calls

} // namespace time
} // namespace ps2ur
