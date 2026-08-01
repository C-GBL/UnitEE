// scene: entities, transforms, culling, render queue (plan section 8: src/scene).
// Loads scene data from the p2b container.
// TODO(spec missing: section 10): p2b container format spec.
#pragma once

namespace ps2ur {
namespace scene {

bool init();
void shutdown();
bool initialized();

} // namespace scene
} // namespace ps2ur
