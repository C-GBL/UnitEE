// BVH construction over static collision triangles (plan section 9, M11
// task 1: "static mesh colliders -> a BVH over triangles").
//
// The BUILDER lives in the runtime rather than only in the Editor for two
// reasons: the host tests need to make a tree without running Unity, and a
// game that generates terrain at runtime should not have to reimplement it.
// The offline path calls exactly this code, so what ships is what was
// tested.
//
// Method: binned SAH over the longest axis, top-down, median fallback. Not
// the best tree a builder can make -- a full sweep SAH over all three axes
// is better -- but it is O(n log n) with a small constant and the difference
// on collision meshes of a few thousand triangles is not worth the build
// time. Where quality does matter is leaf size, which is tuned to the EE's
// cache line rather than to a theoretical optimum.
#pragma once

#include "ps2ur/phys.h"

#include <cstdint>

namespace ps2ur {
namespace phys {

// Triangles per leaf. Four 8-byte triangles plus the node itself is two
// cache lines on the EE, and the traversal reads them all anyway once it has
// paid for the node. Smaller leaves deepen the tree without reducing work.
inline constexpr uint32_t kBvhLeafSize = 4;

// Worst case a node per triangle plus the internal nodes above them.
inline constexpr uint32_t bvh_max_nodes(uint32_t triangle_count)
{
    return triangle_count == 0 ? 1u : triangle_count * 2u + 1u;
}

// Builds a tree over 'triangles' (which is REORDERED in place so each leaf's
// triangles are contiguous -- that contiguity is what lets a leaf store a
// first index and a count instead of an index list).
//
// Returns the node count written, or 0 if the node buffer was too small.
// 'out_nodes' must hold at least bvh_max_nodes(triangle_count).
uint32_t build_bvh(BvhTriangle* triangles, uint32_t triangle_count,
                   const Vec3* vertices, uint32_t vertex_count,
                   BvhNode* out_nodes, uint32_t node_capacity);

// Checks a tree is structurally sound: every child index in range, every
// leaf's triangles inside the array, every parent box containing its
// children, and every triangle reachable exactly once. Used by the tests and
// by the loader, because a malformed BVH from a bad exporter would otherwise
// show up as a wrong collision rather than as an error.
bool validate_bvh(const StaticMesh& mesh, const char** out_error);

} // namespace phys
} // namespace ps2ur
