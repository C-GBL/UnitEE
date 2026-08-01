// phys: raycasts, simple rigid-body integrator, primitive + static mesh
// colliders, CharacterController (plan sections 7.1, 8: src/phys).
// TODO(spec missing: section 9): phys milestone tasks and collider data format.
#pragma once

namespace ps2ur {
namespace phys {

bool init();
void shutdown();
bool initialized();

} // namespace phys
} // namespace ps2ur
