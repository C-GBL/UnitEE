// anim: clip playback, crossfade, additive blending, simplified state machine
// (plan sections 7.1, 8: src/anim).
// TODO(spec missing: section 9): anim milestone tasks; section 10: quantised clip format.
#pragma once

namespace ps2ur {
namespace anim {

bool init();
void shutdown();
bool initialized();

} // namespace anim
} // namespace ps2ur
