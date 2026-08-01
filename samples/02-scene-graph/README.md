# 02-scene-graph

A scene with a real transform hierarchy: parented objects, multiple materials
from the fixed material model (plan section 7.3), several cameras-worth of
content to make culling observable, and `Find`/`GetComponent` usage.

Purpose (plan section 8): exercises the scene exporter (entities, transforms,
component data), the ps2ur scene module (entities, transforms, culling, render
queue -- plan section 8 runtime layout), and parent-child transform propagation
between managed code and the native scene representation.

Acceptance links:
- D5 (plan section 2): conformance assertions for transform math and object
  model queries against Editor play mode.
- D7 (plan section 2): golden images for a multi-object, multi-material scene.

Status: placeholder. Blocked on the scene exporter and runtime scene module.
TODO(spec missing: section 9).
