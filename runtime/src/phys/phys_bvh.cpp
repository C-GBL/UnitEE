#include "ps2ur/phys_bvh.h"

#include <cmath>

namespace ps2ur {
namespace phys {

namespace {

struct Aabb {
    Vec3 bmin{3.4e38f, 3.4e38f, 3.4e38f};
    Vec3 bmax{-3.4e38f, -3.4e38f, -3.4e38f};
};

void expand(Aabb& box, Vec3 p)
{
    box.bmin.x = p.x < box.bmin.x ? p.x : box.bmin.x;
    box.bmin.y = p.y < box.bmin.y ? p.y : box.bmin.y;
    box.bmin.z = p.z < box.bmin.z ? p.z : box.bmin.z;
    box.bmax.x = p.x > box.bmax.x ? p.x : box.bmax.x;
    box.bmax.y = p.y > box.bmax.y ? p.y : box.bmax.y;
    box.bmax.z = p.z > box.bmax.z ? p.z : box.bmax.z;
}

float axis_of(Vec3 v, uint32_t axis)
{
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

Vec3 centroid(const BvhTriangle& tri, const Vec3* verts)
{
    const Vec3 a = verts[tri.v0];
    const Vec3 b = verts[tri.v1];
    const Vec3 c = verts[tri.v2];
    return scale(add(add(a, b), c), 1.0f / 3.0f);
}

Aabb triangle_bounds(const BvhTriangle& tri, const Vec3* verts)
{
    Aabb box;
    expand(box, verts[tri.v0]);
    expand(box, verts[tri.v1]);
    expand(box, verts[tri.v2]);
    return box;
}

// Recursion is bounded by the tree depth, not the triangle count, so an
// explicit stack buys nothing here. Depth is log2(n/leaf) plus the degenerate
// cases, which the median fallback caps.
struct Builder {
    BvhTriangle* triangles = nullptr;
    const Vec3* vertices = nullptr;
    BvhNode* nodes = nullptr;
    uint32_t node_capacity = 0;
    uint32_t node_count = 0;
    bool overflowed = false;

    uint32_t allocate_node()
    {
        if (node_count >= node_capacity) {
            overflowed = true;
            return 0;
        }
        return node_count++;
    }

    void build(uint32_t node_index, uint32_t first, uint32_t count, uint32_t depth)
    {
        Aabb bounds;
        for (uint32_t i = 0; i < count; ++i) {
            const Aabb tri = triangle_bounds(triangles[first + i], vertices);
            expand(bounds, tri.bmin);
            expand(bounds, tri.bmax);
        }
        nodes[node_index].bmin = bounds.bmin;
        nodes[node_index].bmax = bounds.bmax;

        // A depth cap matters: coincident triangles (a common export
        // artefact) have identical centroids and never split, which without
        // this recurses until the stack dies. The median fallback below
        // usually catches it; the cap is the guarantee.
        if (count <= kBvhLeafSize || depth >= 32u) {
            nodes[node_index].first = first;
            nodes[node_index].count = count;
            return;
        }

        // Split the longest axis of the CENTROID bounds, not the triangle
        // bounds: a few large triangles otherwise stretch the box and pick
        // an axis nothing is actually spread along.
        Aabb centroids;
        for (uint32_t i = 0; i < count; ++i) {
            expand(centroids, centroid(triangles[first + i], vertices));
        }
        const Vec3 extent = sub(centroids.bmax, centroids.bmin);
        uint32_t axis = 0;
        if (extent.y > extent.x) {
            axis = 1;
        }
        if (axis_of(extent, 2) > axis_of(extent, axis)) {
            axis = 2;
        }

        uint32_t mid = first;
        const float split = (axis_of(centroids.bmin, axis) +
                             axis_of(centroids.bmax, axis)) *
                            0.5f;
        if (axis_of(extent, axis) > 1e-6f) {
            // In-place partition: the swap is what makes each leaf's
            // triangles contiguous.
            uint32_t i = first;
            uint32_t j = first + count - 1;
            while (i <= j) {
                if (axis_of(centroid(triangles[i], vertices), axis) < split) {
                    ++i;
                } else {
                    const BvhTriangle tmp = triangles[i];
                    triangles[i] = triangles[j];
                    triangles[j] = tmp;
                    if (j == first) {
                        break;
                    }
                    --j;
                }
            }
            mid = i;
        }
        if (mid == first || mid == first + count) {
            // Everything landed on one side (identical centroids, or a
            // split plane exactly on them). Halve by count instead: a worse
            // tree, but a finite one.
            mid = first + count / 2u;
        }

        const uint32_t left = allocate_node();
        const uint32_t right = allocate_node();
        if (overflowed) {
            // Degrade to a leaf rather than write outside the buffer. The
            // caller sees node_count == 0 and reports the failure.
            nodes[node_index].first = first;
            nodes[node_index].count = count;
            return;
        }
        nodes[node_index].first = left;
        nodes[node_index].count = 0; // internal
        build(left, first, mid - first, depth + 1);
        build(right, mid, first + count - mid, depth + 1);
        (void)right; // right is always left + 1 by construction
    }
};

bool contains(Vec3 outer_min, Vec3 outer_max, Vec3 inner_min, Vec3 inner_max)
{
    // A hair of tolerance: the parent's bounds are computed from the same
    // triangles, so equality is expected and float rounding can make a child
    // bound sit a bit outside.
    constexpr float kSlack = 1e-3f;
    return inner_min.x >= outer_min.x - kSlack &&
           inner_min.y >= outer_min.y - kSlack &&
           inner_min.z >= outer_min.z - kSlack &&
           inner_max.x <= outer_max.x + kSlack &&
           inner_max.y <= outer_max.y + kSlack &&
           inner_max.z <= outer_max.z + kSlack;
}

} // namespace

uint32_t build_bvh(BvhTriangle* triangles, uint32_t triangle_count,
                   const Vec3* vertices, uint32_t vertex_count,
                   BvhNode* out_nodes, uint32_t node_capacity)
{
    if (out_nodes == nullptr || node_capacity == 0) {
        return 0;
    }
    if (triangle_count == 0) {
        // An empty tree is legal and useful: a scene with primitives only.
        // One inverted-bounds node means every query misses immediately.
        out_nodes[0].bmin = Vec3{3.4e38f, 3.4e38f, 3.4e38f};
        out_nodes[0].bmax = Vec3{-3.4e38f, -3.4e38f, -3.4e38f};
        out_nodes[0].first = 0;
        out_nodes[0].count = 0;
        return 1;
    }
    if (triangles == nullptr || vertices == nullptr) {
        return 0;
    }
    // An out-of-range index would read past the vertex array during the
    // build, so it is rejected here rather than trusted.
    for (uint32_t i = 0; i < triangle_count; ++i) {
        if (triangles[i].v0 >= vertex_count || triangles[i].v1 >= vertex_count ||
            triangles[i].v2 >= vertex_count) {
            return 0;
        }
    }

    Builder builder;
    builder.triangles = triangles;
    builder.vertices = vertices;
    builder.nodes = out_nodes;
    builder.node_capacity = node_capacity;
    const uint32_t root = builder.allocate_node();
    if (builder.overflowed) {
        return 0;
    }
    builder.build(root, 0, triangle_count, 0);
    return builder.overflowed ? 0 : builder.node_count;
}

bool validate_bvh(const StaticMesh& mesh, const char** out_error)
{
    const char* error = nullptr;
    // A tree with no nodes is not the same as an empty tree; the builder
    // always emits at least a root.
    if (mesh.nodes == nullptr || mesh.node_count == 0) {
        error = "bvh has no nodes";
    } else if (mesh.triangle_count > 0 &&
               (mesh.triangles == nullptr || mesh.vertices == nullptr)) {
        error = "bvh has triangles but no triangle or vertex array";
    }
    if (error == nullptr) {
        for (uint32_t i = 0; i < mesh.triangle_count; ++i) {
            const BvhTriangle& tri = mesh.triangles[i];
            if (tri.v0 >= mesh.vertex_count || tri.v1 >= mesh.vertex_count ||
                tri.v2 >= mesh.vertex_count) {
                error = "bvh triangle indexes a vertex that does not exist";
                break;
            }
        }
    }
    // Every triangle must be covered by exactly one leaf. A gap means
    // geometry silently has no collision; an overlap means a triangle is
    // tested twice and contacts get double-counted.
    static uint8_t seen[65536];
    if (error == nullptr && mesh.triangle_count <= sizeof(seen)) {
        for (uint32_t i = 0; i < mesh.triangle_count; ++i) {
            seen[i] = 0;
        }
        for (uint32_t i = 0; i < mesh.node_count && error == nullptr; ++i) {
            const BvhNode& node = mesh.nodes[i];
            if (node.count == 0) {
                if (node.first + 1u >= mesh.node_count) {
                    error = "bvh internal node points outside the node array";
                    break;
                }
                const BvhNode& left = mesh.nodes[node.first];
                const BvhNode& right = mesh.nodes[node.first + 1];
                if (!contains(node.bmin, node.bmax, left.bmin, left.bmax) ||
                    !contains(node.bmin, node.bmax, right.bmin, right.bmax)) {
                    error = "bvh child bounds escape the parent";
                    break;
                }
                continue;
            }
            if (node.first + node.count > mesh.triangle_count) {
                error = "bvh leaf points outside the triangle array";
                break;
            }
            for (uint32_t t = 0; t < node.count; ++t) {
                if (seen[node.first + t] != 0) {
                    error = "bvh triangle appears in more than one leaf";
                    break;
                }
                seen[node.first + t] = 1;
            }
        }
        if (error == nullptr) {
            for (uint32_t i = 0; i < mesh.triangle_count; ++i) {
                if (seen[i] == 0) {
                    error = "bvh leaves do not cover every triangle";
                    break;
                }
            }
        }
    }
    if (out_error != nullptr) {
        *out_error = error == nullptr ? "" : error;
    }
    return error == nullptr;
}

} // namespace phys
} // namespace ps2ur
