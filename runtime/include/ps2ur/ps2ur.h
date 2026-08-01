// ps2ur -- the PS2-side engine runtime (plan section 8: runtime/).
// Umbrella header: pulls in every public subsystem header.
#pragma once

#define PS2UR_VERSION_MAJOR 0
#define PS2UR_VERSION_MINOR 1
#define PS2UR_VERSION_PATCH 0
#define PS2UR_VERSION_STRING "0.1.0"

#include "ps2ur/log.h"
#include "ps2ur/assert.h"
#include "ps2ur/alloc.h"
#include "ps2ur/time.h"
#include "ps2ur/math.h"
#include "ps2ur/platform.h"
#include "ps2ur/gfx.h"
#include "ps2ur/vu.h"
#include "ps2ur/scene.h"
#include "ps2ur/anim.h"
#include "ps2ur/phys.h"
#include "ps2ur/audio.h"
#include "ps2ur/io.h"
#include "ps2ur/input.h"
#include "ps2ur/bridge.h"

namespace ps2ur {

inline const char* version_string() { return PS2UR_VERSION_STRING; }

} // namespace ps2ur
