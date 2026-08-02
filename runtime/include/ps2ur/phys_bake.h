// Reading the baked PHYS section (plan section 9, M11 task 1; format in
// docs/formats/p2b-container.md).
//
// ZERO COPY, like every other section: the BVH nodes, triangles and vertices
// are used straight out of the container's bytes, so the .p2b buffer must
// outlive the physics world. That is the same contract meshes and animation
// clips already have.
//
// The one thing this cannot do zero-copy is the collider table, because a
// Collider carries runtime state (its body index, whether it is enabled)
// that the file has no business owning. Colliders are therefore COPIED into
// the module's table on load, which is also what makes a scene reload
// replace them cleanly.
#pragma once

#include "ps2ur/p2b.h"
#include "ps2ur/phys.h"

namespace ps2ur {
namespace phys {

// What a load produced, for reporting and for tests.
struct BakeInfo {
    uint32_t collider_count = 0;
    uint32_t triangle_count = 0;
    uint32_t node_count = 0;
    uint32_t vertex_count = 0;
};

// Points 'out_mesh' at the container's BVH and appends the file's primitive
// colliders to the module's table. Returns false with *out_error set on any
// structural problem -- and the module is left untouched, so a bad file
// cannot half-load a collision world.
//
// Alignment note: BvhNode and Vec3 are read in place, so the section payload
// must be 16-byte aligned. The container guarantees 2048-byte alignment for
// every payload, which covers it.
bool load_physics(const io::P2bFile& file, StaticMesh* out_mesh,
                  BakeInfo* out_info, const char** out_error);

} // namespace phys
} // namespace ps2ur
