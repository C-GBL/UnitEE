// World queries: raycast, spherecast, capsule sweep and overlap, against the
// baked static BVH and the dynamic collider table (plan section 9, M11 task
// 2).
//
// Traversal shape, and why. Every query walks the tree with an EXPLICIT
// stack rather than recursing: the depth is data-dependent (a bad export can
// produce a deep tree) and the EE's stack is 2.5 MB shared with everything
// else, so a bounded array is the honest structure. The stack is sized to
// the builder's depth cap.
//
// The ray traversal is front-to-back and shrinks max_t on every hit, which
// is what makes a closest-hit query cheap: whole subtrees fail the slab test
// once something nearer has been found.
#include "ps2ur/phys.h"

#include "phys_internal.h"

#include <cmath>

namespace ps2ur {
namespace phys {

namespace {

// The builder caps depth at 32, so a node and its sibling at every level fit
// in 64. Overflow is impossible by construction rather than by hope; the
// guard exists because "by construction" depends on a file we did not write.
constexpr uint32_t kStackSize = 64;

constexpr float kEpsilon = 1e-6f;
constexpr float kInfinity = 3.4e38f;

Vec3 reciprocal(Vec3 v)
{
    // A zero component becomes a large finite number rather than an infinity:
    // the EE does not handle infinities the way IEEE 754 says it should
    // (section 3.1), and a NaN out of 0 * inf inside the slab test would
    // silently drop hits. A huge finite value gives the same answer.
    constexpr float kHuge = 1e30f;
    return Vec3{fabsf(v.x) > kEpsilon ? 1.0f / v.x : kHuge,
                fabsf(v.y) > kEpsilon ? 1.0f / v.y : kHuge,
                fabsf(v.z) > kEpsilon ? 1.0f / v.z : kHuge};
}

bool layer_allowed(uint32_t mask, uint8_t layer)
{
    return (mask & (1u << (layer & 31u))) != 0u;
}

} // namespace

// ---- static raycast --------------------------------------------------------

bool raycast_static(Vec3 origin, Vec3 direction, float max_distance,
                    uint32_t mask, RaycastHit* out_hit)
{
    const StaticMesh* mesh = static_mesh();
    if (mesh == nullptr || mesh->node_count == 0) {
        return false;
    }
    const Vec3 inv_dir = reciprocal(direction);

    uint32_t stack[kStackSize];
    uint32_t depth = 0;
    stack[depth++] = 0;

    float best_t = max_distance;
    bool found = false;
    Vec3 best_normal{0, 1, 0};
    int32_t best_triangle = -1;

    while (depth > 0) {
        const uint32_t index = stack[--depth];
        const BvhNode& node = mesh->nodes[index];
        g_stats.bvh_nodes_visited++;

        float node_t = 0.0f;
        if (!ray_aabb(origin, inv_dir, node.bmin, node.bmax, best_t, &node_t)) {
            continue;
        }
        // A box that starts beyond the best hit cannot contain a better one.
        if (node_t > best_t) {
            continue;
        }
        if (node.count == 0) {
            if (depth + 2 <= kStackSize) {
                stack[depth++] = node.first;
                stack[depth++] = node.first + 1;
            }
            continue;
        }
        for (uint32_t i = 0; i < node.count; ++i) {
            const uint32_t tri_index = node.first + i;
            const BvhTriangle& tri = mesh->triangles[tri_index];
            if (!layer_allowed(mask, tri.layer)) {
                continue;
            }
            g_stats.triangle_tests++;
            float t = 0.0f;
            Vec3 normal{0, 1, 0};
            // Two-sided: a collision mesh's winding is whatever the artist
            // left it as, and a one-sided floor a player can fall through
            // from below is a bug report, not a feature.
            if (ray_triangle(origin, direction, mesh->vertices[tri.v0],
                             mesh->vertices[tri.v1], mesh->vertices[tri.v2],
                             /*cull=*/false, &t, &normal) &&
                t < best_t) {
                best_t = t;
                best_normal = normal;
                best_triangle = static_cast<int32_t>(tri_index);
                found = true;
            }
        }
    }

    if (found && out_hit != nullptr) {
        out_hit->distance = best_t;
        out_hit->point = add(origin, scale(direction, best_t));
        out_hit->normal = best_normal;
        out_hit->collider = -1;
        out_hit->triangle = best_triangle;
        out_hit->layer = best_triangle >= 0
                             ? mesh->triangles[best_triangle].layer
                             : 0;
    }
    return found;
}

// ---- static triangle gathering --------------------------------------------

uint32_t gather_static_triangles(Vec3 bmin, Vec3 bmax, uint32_t mask,
                                 uint32_t* out_indices, uint32_t capacity)
{
    const StaticMesh* mesh = static_mesh();
    if (mesh == nullptr || mesh->node_count == 0 || out_indices == nullptr) {
        return 0;
    }
    uint32_t stack[kStackSize];
    uint32_t depth = 0;
    stack[depth++] = 0;
    uint32_t found = 0;

    while (depth > 0) {
        const uint32_t index = stack[--depth];
        const BvhNode& node = mesh->nodes[index];
        g_stats.bvh_nodes_visited++;

        if (node.bmin.x > bmax.x || node.bmax.x < bmin.x ||
            node.bmin.y > bmax.y || node.bmax.y < bmin.y ||
            node.bmin.z > bmax.z || node.bmax.z < bmin.z) {
            continue;
        }
        if (node.count == 0) {
            if (depth + 2 <= kStackSize) {
                stack[depth++] = node.first;
                stack[depth++] = node.first + 1;
            }
            continue;
        }
        for (uint32_t i = 0; i < node.count; ++i) {
            const uint32_t tri_index = node.first + i;
            if (!layer_allowed(mask, mesh->triangles[tri_index].layer)) {
                continue;
            }
            if (found < capacity) {
                out_indices[found] = tri_index;
            }
            // Counted even when it does not fit, so the caller can tell the
            // difference between "none" and "more than you asked for".
            ++found;
        }
    }
    return found;
}

// ---- capsule sweep ---------------------------------------------------------

bool sweep_capsule(const CapsuleShape& capsule, Vec3 motion, uint32_t mask,
                   float* out_fraction, Vec3* out_normal, int32_t* out_triangle)
{
    const StaticMesh* mesh = static_mesh();
    if (out_fraction != nullptr) {
        *out_fraction = 1.0f;
    }
    if (mesh == nullptr || mesh->node_count == 0) {
        return false;
    }
    const float distance = length(motion);
    if (distance < kEpsilon) {
        return false;
    }

    // Conservative advancement over a swept AABB. A full continuous capsule
    // vs triangle solve is a quartic; this instead gathers everything the
    // swept volume could touch and binary-searches the first fraction at
    // which the capsule is clear. Fewer edge cases, no root finding, and the
    // error is bounded by the iteration count rather than by conditioning.
    Vec3 bmin = capsule.a;
    Vec3 bmax = capsule.a;
    const Vec3 corners[4] = {capsule.a, capsule.b, add(capsule.a, motion),
                             add(capsule.b, motion)};
    for (uint32_t i = 0; i < 4; ++i) {
        bmin.x = corners[i].x < bmin.x ? corners[i].x : bmin.x;
        bmin.y = corners[i].y < bmin.y ? corners[i].y : bmin.y;
        bmin.z = corners[i].z < bmin.z ? corners[i].z : bmin.z;
        bmax.x = corners[i].x > bmax.x ? corners[i].x : bmax.x;
        bmax.y = corners[i].y > bmax.y ? corners[i].y : bmax.y;
        bmax.z = corners[i].z > bmax.z ? corners[i].z : bmax.z;
    }
    const Vec3 pad{capsule.radius, capsule.radius, capsule.radius};
    bmin = sub(bmin, pad);
    bmax = add(bmax, pad);

    uint32_t candidates[kMaxTouchedTriangles];
    const uint32_t count =
        gather_static_triangles(bmin, bmax, mask, candidates,
                                kMaxTouchedTriangles);
    const uint32_t tested = count < kMaxTouchedTriangles ? count
                                                         : kMaxTouchedTriangles;
    if (tested == 0) {
        return false;
    }

    // Already overlapping at t=0: report a zero-fraction hit so the caller
    // depenetrates rather than sweeping out of a wall it is inside.
    //
    // MEANINGFULLY overlapping, though. A shape resting exactly on a surface
    // is "touching" at zero depth, and treating that as a blocking overlap
    // pins anything standing on the ground in place forever -- it can never
    // start a move because it is already in contact with the floor it rests
    // on.
    constexpr float kTouchTolerance = 1e-3f;
    for (uint32_t i = 0; i < tested; ++i) {
        const BvhTriangle& tri = mesh->triangles[candidates[i]];
        Vec3 normal;
        float depth;
        g_stats.triangle_tests++;
        if (capsule_triangle(capsule, mesh->vertices[tri.v0],
                             mesh->vertices[tri.v1], mesh->vertices[tri.v2],
                             &normal, &depth) &&
            depth > kTouchTolerance) {
            if (out_fraction != nullptr) {
                *out_fraction = 0.0f;
            }
            if (out_normal != nullptr) {
                *out_normal = normal;
            }
            if (out_triangle != nullptr) {
                *out_triangle = static_cast<int32_t>(candidates[i]);
            }
            return true;
        }
    }

    // Step forward at a resolution finer than the capsule radius, so nothing
    // thinner than the capsule can be skipped over. That is the anti-
    // tunnelling guarantee the acceptance test checks.
    const float step = capsule.radius > kEpsilon ? capsule.radius * 0.5f : 0.05f;
    uint32_t steps = static_cast<uint32_t>(distance / step) + 1u;
    if (steps > 64u) {
        steps = 64u; // a very long sweep degrades in accuracy, not safety
    }

    for (uint32_t s = 1; s <= steps; ++s) {
        const float t = static_cast<float>(s) / static_cast<float>(steps);
        CapsuleShape moved = capsule;
        const Vec3 offset = scale(motion, t);
        moved.a = add(capsule.a, offset);
        moved.b = add(capsule.b, offset);

        for (uint32_t i = 0; i < tested; ++i) {
            const BvhTriangle& tri = mesh->triangles[candidates[i]];
            Vec3 normal;
            float depth;
            g_stats.triangle_tests++;
            if (!capsule_triangle(moved, mesh->vertices[tri.v0],
                                  mesh->vertices[tri.v1], mesh->vertices[tri.v2],
                                  &normal, &depth)) {
                continue;
            }
            // Refine between the last clear step and this one. Eight
            // bisections take the residual to 1/256 of a step, which is far
            // below the skin width the caller keeps anyway.
            float lo = static_cast<float>(s - 1) / static_cast<float>(steps);
            float hi = t;
            for (uint32_t iter = 0; iter < 8; ++iter) {
                const float mid = (lo + hi) * 0.5f;
                CapsuleShape probe = capsule;
                const Vec3 probe_offset = scale(motion, mid);
                probe.a = add(capsule.a, probe_offset);
                probe.b = add(capsule.b, probe_offset);
                bool touching = false;
                for (uint32_t j = 0; j < tested && !touching; ++j) {
                    const BvhTriangle& t2 = mesh->triangles[candidates[j]];
                    g_stats.triangle_tests++;
                    touching = capsule_triangle(probe, mesh->vertices[t2.v0],
                                                mesh->vertices[t2.v1],
                                                mesh->vertices[t2.v2], &normal,
                                                &depth);
                }
                if (touching) {
                    hi = mid;
                } else {
                    lo = mid;
                }
            }
            if (out_fraction != nullptr) {
                *out_fraction = lo;
            }
            if (out_normal != nullptr) {
                *out_normal = normal;
            }
            if (out_triangle != nullptr) {
                *out_triangle = static_cast<int32_t>(candidates[i]);
            }
            return true;
        }
    }
    return false;
}

// ---- public queries --------------------------------------------------------

bool raycast(Vec3 origin, Vec3 direction, float max_distance, uint32_t mask,
             RaycastHit* out_hit)
{
    const float len = length(direction);
    if (len < kEpsilon || max_distance <= 0.0f) {
        return false;
    }
    const Vec3 dir = scale(direction, 1.0f / len);

    RaycastHit best;
    bool found = raycast_static(origin, dir, max_distance, mask, &best);
    float best_t = found ? best.distance : max_distance;

    // Dynamic colliders are tested linearly. With kMaxColliders at 256 and a
    // ray being rare compared to the step, a broadphase over them would cost
    // more to maintain than it saves; if that stops being true the profiler
    // will say so.
    for (uint32_t i = 0; i < collider_count(); ++i) {
        const Collider& c = collider_const(i);
        if (!c.enabled || !layer_allowed(mask, c.layer)) {
            continue;
        }
        g_stats.pair_tests++;
        float t = 0.0f;
        Vec3 normal{0, 1, 0};
        if (!raycast_collider(i, origin, dir, best_t, &t, &normal)) {
            continue;
        }
        if (t < best_t) {
            best_t = t;
            best.distance = t;
            best.point = add(origin, scale(dir, t));
            best.normal = normal;
            best.collider = static_cast<int32_t>(i);
            best.triangle = -1;
            best.layer = c.layer;
            found = true;
        }
    }

    if (found && out_hit != nullptr) {
        *out_hit = best;
    }
    return found;
}

bool spherecast(Vec3 origin, float radius, Vec3 direction, float max_distance,
                uint32_t mask, RaycastHit* out_hit)
{
    const float len = length(direction);
    if (len < kEpsilon || max_distance <= 0.0f) {
        return false;
    }
    const Vec3 dir = scale(direction, 1.0f / len);
    if (radius <= kEpsilon) {
        return raycast(origin, dir, max_distance, mask, out_hit);
    }

    // A sphere is a capsule with a zero-length segment, so the sweep above
    // covers it exactly rather than by approximation.
    CapsuleShape sphere;
    sphere.a = origin;
    sphere.b = origin;
    sphere.radius = radius;

    float fraction = 1.0f;
    Vec3 normal{0, 1, 0};
    int32_t triangle = -1;
    if (!sweep_capsule(sphere, scale(dir, max_distance), mask, &fraction,
                       &normal, &triangle)) {
        return false;
    }
    if (out_hit != nullptr) {
        out_hit->distance = fraction * max_distance;
        out_hit->point = add(origin, scale(dir, out_hit->distance));
        out_hit->normal = normal;
        out_hit->collider = -1;
        out_hit->triangle = triangle;
        const StaticMesh* mesh = static_mesh();
        out_hit->layer = (triangle >= 0 && mesh != nullptr)
                             ? mesh->triangles[triangle].layer
                             : 0;
    }
    return true;
}

bool overlaps_static(Vec3 center, float radius, uint32_t mask)
{
    const StaticMesh* mesh = static_mesh();
    if (mesh == nullptr || mesh->node_count == 0) {
        return false;
    }
    const Vec3 pad{radius, radius, radius};
    uint32_t candidates[kMaxTouchedTriangles];
    const uint32_t count = gather_static_triangles(
        sub(center, pad), add(center, pad), mask, candidates,
        kMaxTouchedTriangles);
    const uint32_t tested = count < kMaxTouchedTriangles ? count
                                                         : kMaxTouchedTriangles;
    for (uint32_t i = 0; i < tested; ++i) {
        const BvhTriangle& tri = mesh->triangles[candidates[i]];
        g_stats.triangle_tests++;
        if (sphere_triangle(center, radius, mesh->vertices[tri.v0],
                            mesh->vertices[tri.v1], mesh->vertices[tri.v2],
                            nullptr, nullptr)) {
            return true;
        }
    }
    return false;
}

uint32_t overlap_sphere(Vec3 center, float radius, uint32_t mask,
                        int32_t* out_colliders, uint32_t capacity)
{
    uint32_t found = 0;
    for (uint32_t i = 0; i < collider_count(); ++i) {
        const Collider& c = collider_const(i);
        if (!c.enabled || !layer_allowed(mask, c.layer)) {
            continue;
        }
        g_stats.pair_tests++;
        if (!collider_overlaps_sphere(i, center, radius)) {
            continue;
        }
        if (out_colliders != nullptr && found < capacity) {
            out_colliders[found] = static_cast<int32_t>(i);
        }
        ++found;
    }
    return found;
}

} // namespace phys
} // namespace ps2ur
