// Intersection primitives (plan section 9, M11 task 2).
//
// Everything here is pure maths over floats: no world state, no allocation,
// no branches on anything but the geometry. That is deliberate -- these are
// the functions the tests can pin exactly, and every query above them is
// assembled out of these rather than re-deriving the same algebra inline.
//
// Convention for every *_normal out-parameter: the normal points from the
// SECOND shape towards the FIRST, so moving shape A along +normal by 'depth'
// separates the pair. Getting this backwards is the classic way to make
// objects suck together instead of pushing apart, so it is stated once here
// and never varied.
#include "ps2ur/phys.h"

#include <cmath>

namespace ps2ur {
namespace phys {

namespace {

// Below this a vector is treated as having no direction. Chosen well above
// the EE's float noise floor: the hardware is not IEEE 754 and denormals do
// not behave, so anything smaller is not a number worth normalising.
constexpr float kEpsilon = 1e-6f;

float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// A unit vector, or the fallback when the input has no length. Callers pass
// a fallback that makes sense for their contact rather than getting a zero
// normal they then have to check.
Vec3 safe_normalize(Vec3 v, Vec3 fallback)
{
    const float len_sq = length_sq(v);
    if (len_sq < kEpsilon * kEpsilon) {
        return fallback;
    }
    return scale(v, 1.0f / sqrtf(len_sq));
}

} // namespace

// ---- rays ------------------------------------------------------------------

bool ray_triangle(Vec3 origin, Vec3 direction, Vec3 v0, Vec3 v1, Vec3 v2,
                  bool cull, float* out_t, Vec3* out_normal)
{
    // Moller-Trumbore: no precomputed plane, one cross product and two dots
    // to get the barycentrics. Chosen over a plane test because collision
    // triangles are indexed and storing a plane per triangle would cost more
    // memory than the arithmetic saves.
    const Vec3 e1 = sub(v1, v0);
    const Vec3 e2 = sub(v2, v0);
    const Vec3 p = cross(direction, e2);
    const float det = dot(e1, p);

    if (cull) {
        if (det < kEpsilon) {
            return false; // back-facing or parallel
        }
    } else if (det > -kEpsilon && det < kEpsilon) {
        return false; // parallel to the plane
    }

    const float inv_det = 1.0f / det;
    const Vec3 t_vec = sub(origin, v0);
    const float u = dot(t_vec, p) * inv_det;
    if (u < 0.0f || u > 1.0f) {
        return false;
    }
    const Vec3 q = cross(t_vec, e1);
    const float v = dot(direction, q) * inv_det;
    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }
    const float t = dot(e2, q) * inv_det;
    if (t < 0.0f) {
        return false; // behind the origin
    }
    if (out_t != nullptr) {
        *out_t = t;
    }
    if (out_normal != nullptr) {
        // Face normal, flipped to oppose the ray so a hit from behind an
        // unculled triangle still reports something the caller can slide on.
        Vec3 n = safe_normalize(cross(e1, e2), Vec3{0, 1, 0});
        if (dot(n, direction) > 0.0f) {
            n = scale(n, -1.0f);
        }
        *out_normal = n;
    }
    return true;
}

bool ray_aabb(Vec3 origin, Vec3 inv_dir, Vec3 bmin, Vec3 bmax, float max_t,
              float* out_t)
{
    // Branchless slab test. min/max ordering handles a negative inv_dir
    // without a sign test, and an axis-parallel ray produces an infinity
    // that compares correctly rather than a NaN -- as long as the origin is
    // not exactly on the slab, which is why the traversal nudges rather than
    // relying on it.
    const float tx1 = (bmin.x - origin.x) * inv_dir.x;
    const float tx2 = (bmax.x - origin.x) * inv_dir.x;
    float tmin = tx1 < tx2 ? tx1 : tx2;
    float tmax = tx1 < tx2 ? tx2 : tx1;

    const float ty1 = (bmin.y - origin.y) * inv_dir.y;
    const float ty2 = (bmax.y - origin.y) * inv_dir.y;
    const float tymin = ty1 < ty2 ? ty1 : ty2;
    const float tymax = ty1 < ty2 ? ty2 : ty1;
    tmin = tmin > tymin ? tmin : tymin;
    tmax = tmax < tymax ? tmax : tymax;

    const float tz1 = (bmin.z - origin.z) * inv_dir.z;
    const float tz2 = (bmax.z - origin.z) * inv_dir.z;
    const float tzmin = tz1 < tz2 ? tz1 : tz2;
    const float tzmax = tz1 < tz2 ? tz2 : tz1;
    tmin = tmin > tzmin ? tmin : tzmin;
    tmax = tmax < tzmax ? tmax : tzmax;

    if (tmax < 0.0f || tmin > tmax || tmin > max_t) {
        return false;
    }
    if (out_t != nullptr) {
        *out_t = tmin < 0.0f ? 0.0f : tmin; // origin inside the box
    }
    return true;
}

// ---- closest points --------------------------------------------------------

Vec3 closest_point_on_segment(Vec3 a, Vec3 b, Vec3 p)
{
    const Vec3 ab = sub(b, a);
    const float len_sq = length_sq(ab);
    if (len_sq < kEpsilon) {
        return a; // degenerate segment: a point
    }
    const float t = clampf(dot(sub(p, a), ab) / len_sq, 0.0f, 1.0f);
    return add(a, scale(ab, t));
}

Vec3 closest_point_on_triangle(Vec3 p, Vec3 v0, Vec3 v1, Vec3 v2)
{
    // Ericson, Real-Time Collision Detection 5.1.5: check the three vertex
    // regions and the three edge regions before falling through to the face.
    // Done this way it is all dot products -- no division until the face
    // case, and no square roots at all.
    const Vec3 ab = sub(v1, v0);
    const Vec3 ac = sub(v2, v0);
    const Vec3 ap = sub(p, v0);
    const float d1 = dot(ab, ap);
    const float d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        return v0;
    }

    const Vec3 bp = sub(p, v1);
    const float d3 = dot(ab, bp);
    const float d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {
        return v1;
    }

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float denom = d1 - d3;
        const float t = denom > kEpsilon ? d1 / denom : 0.0f;
        return add(v0, scale(ab, t));
    }

    const Vec3 cp = sub(p, v2);
    const float d5 = dot(ab, cp);
    const float d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
        return v2;
    }

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float denom = d2 - d6;
        const float t = denom > kEpsilon ? d2 / denom : 0.0f;
        return add(v0, scale(ac, t));
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const float denom = (d4 - d3) + (d5 - d6);
        const float t = denom > kEpsilon ? (d4 - d3) / denom : 0.0f;
        return add(v1, scale(sub(v2, v1), t));
    }

    const float denom = va + vb + vc;
    if (denom < kEpsilon) {
        return v0; // degenerate triangle
    }
    const float inv = 1.0f / denom;
    return add(v0, add(scale(ab, vb * inv), scale(ac, vc * inv)));
}

void closest_points_segments(Vec3 p1, Vec3 q1, Vec3 p2, Vec3 q2, Vec3* out_c1,
                             Vec3* out_c2)
{
    // Ericson 5.1.9. The parallel case is the one that bites: the general
    // solution divides by a determinant that goes to zero, so it is detected
    // and handled rather than clamped after the fact.
    const Vec3 d1 = sub(q1, p1);
    const Vec3 d2 = sub(q2, p2);
    const Vec3 r = sub(p1, p2);
    const float a = length_sq(d1);
    const float e = length_sq(d2);
    const float f = dot(d2, r);

    float s = 0.0f;
    float t = 0.0f;

    if (a < kEpsilon && e < kEpsilon) {
        // Both degenerate: two points.
        if (out_c1 != nullptr) {
            *out_c1 = p1;
        }
        if (out_c2 != nullptr) {
            *out_c2 = p2;
        }
        return;
    }
    if (a < kEpsilon) {
        t = clampf(f / e, 0.0f, 1.0f);
    } else {
        const float c = dot(d1, r);
        if (e < kEpsilon) {
            s = clampf(-c / a, 0.0f, 1.0f);
        } else {
            const float b = dot(d1, d2);
            const float denom = a * e - b * b;
            s = denom > kEpsilon ? clampf((b * f - c * e) / denom, 0.0f, 1.0f)
                                 : 0.0f;
            t = (b * s + f) / e;
            // Re-clamp s once t has been clamped, or the pair is not
            // actually the closest one on both segments.
            if (t < 0.0f) {
                t = 0.0f;
                s = clampf(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = clampf((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    if (out_c1 != nullptr) {
        *out_c1 = add(p1, scale(d1, s));
    }
    if (out_c2 != nullptr) {
        *out_c2 = add(p2, scale(d2, t));
    }
}

// ---- overlaps --------------------------------------------------------------

bool sphere_triangle(Vec3 center, float radius, Vec3 v0, Vec3 v1, Vec3 v2,
                     Vec3* out_normal, float* out_depth)
{
    const Vec3 closest = closest_point_on_triangle(center, v0, v1, v2);
    const Vec3 delta = sub(center, closest);
    const float dist_sq = length_sq(delta);
    if (dist_sq > radius * radius) {
        return false;
    }
    // A centre exactly on the face has no direction to push along, so fall
    // back to the face normal -- which is the right answer, not a guess.
    const Vec3 face = safe_normalize(cross(sub(v1, v0), sub(v2, v0)), Vec3{0, 1, 0});
    const float dist = sqrtf(dist_sq);
    if (out_normal != nullptr) {
        *out_normal = dist > kEpsilon ? scale(delta, 1.0f / dist) : face;
    }
    if (out_depth != nullptr) {
        *out_depth = radius - dist;
    }
    return true;
}

bool capsule_triangle(const CapsuleShape& capsule, Vec3 v0, Vec3 v1, Vec3 v2,
                      Vec3* out_normal, float* out_depth)
{
    // Find the point on the capsule's axis closest to the triangle, then
    // reduce to sphere-vs-triangle there. The first guess comes from
    // intersecting the axis with the triangle plane, which is exact for a
    // face contact and a good starting point for an edge one.
    const Vec3 face = safe_normalize(cross(sub(v1, v0), sub(v2, v0)), Vec3{0, 1, 0});
    const Vec3 axis = sub(capsule.b, capsule.a);
    const float denom = dot(face, axis);

    Vec3 reference;
    if (denom > -kEpsilon && denom < kEpsilon) {
        // Axis parallel to the plane: any point projects the same way.
        reference = closest_point_on_triangle(capsule.a, v0, v1, v2);
    } else {
        const float t = clampf(dot(face, sub(v0, capsule.a)) / denom, 0.0f, 1.0f);
        const Vec3 on_axis = add(capsule.a, scale(axis, t));
        reference = closest_point_on_triangle(on_axis, v0, v1, v2);
    }
    const Vec3 center = closest_point_on_segment(capsule.a, capsule.b, reference);
    return sphere_triangle(center, capsule.radius, v0, v1, v2, out_normal,
                           out_depth);
}

bool sphere_sphere(Vec3 c0, float r0, Vec3 c1, float r1, Vec3* out_normal,
                   float* out_depth)
{
    const Vec3 delta = sub(c0, c1);
    const float dist_sq = length_sq(delta);
    const float sum = r0 + r1;
    if (dist_sq > sum * sum) {
        return false;
    }
    const float dist = sqrtf(dist_sq);
    if (out_normal != nullptr) {
        // Concentric spheres: pick an axis rather than divide by zero. Any
        // choice is arbitrary; +Y at least pushes things apart upward, which
        // is the least surprising outcome in a gravity-having world.
        *out_normal = dist > kEpsilon ? scale(delta, 1.0f / dist) : Vec3{0, 1, 0};
    }
    if (out_depth != nullptr) {
        *out_depth = sum - dist;
    }
    return true;
}

bool sphere_box(Vec3 center, float radius, const BoxShape& box,
                Vec3* out_normal, float* out_depth)
{
    // Work in the box's local frame: the inverse of a unit quaternion is its
    // conjugate, so this costs two rotations and no matrix.
    const Quat inv = quat_conjugate(box.rotation);
    const Vec3 local = quat_rotate(inv, sub(center, box.center));
    const Vec3 h = box.half_extents;

    const Vec3 clamped{clampf(local.x, -h.x, h.x), clampf(local.y, -h.y, h.y),
                       clampf(local.z, -h.z, h.z)};
    const Vec3 delta = sub(local, clamped);
    const float dist_sq = length_sq(delta);

    if (dist_sq > radius * radius) {
        return false;
    }
    Vec3 local_normal;
    float depth;
    if (dist_sq > kEpsilon * kEpsilon) {
        const float dist = sqrtf(dist_sq);
        local_normal = scale(delta, 1.0f / dist);
        depth = radius - dist;
    } else {
        // Centre inside the box: push out along the least-penetrated axis,
        // which is the shortest way out and the one that does not teleport
        // an object through the box.
        const float dx = h.x - fabsf(local.x);
        const float dy = h.y - fabsf(local.y);
        const float dz = h.z - fabsf(local.z);
        if (dx <= dy && dx <= dz) {
            local_normal = Vec3{local.x >= 0.0f ? 1.0f : -1.0f, 0, 0};
            depth = dx + radius;
        } else if (dy <= dz) {
            local_normal = Vec3{0, local.y >= 0.0f ? 1.0f : -1.0f, 0};
            depth = dy + radius;
        } else {
            local_normal = Vec3{0, 0, local.z >= 0.0f ? 1.0f : -1.0f};
            depth = dz + radius;
        }
    }
    if (out_normal != nullptr) {
        *out_normal = quat_rotate(box.rotation, local_normal);
    }
    if (out_depth != nullptr) {
        *out_depth = depth;
    }
    return true;
}

bool sphere_capsule(Vec3 center, float radius, const CapsuleShape& capsule,
                    Vec3* out_normal, float* out_depth)
{
    const Vec3 on_axis = closest_point_on_segment(capsule.a, capsule.b, center);
    return sphere_sphere(center, radius, on_axis, capsule.radius, out_normal,
                         out_depth);
}

bool capsule_capsule(const CapsuleShape& a, const CapsuleShape& b,
                     Vec3* out_normal, float* out_depth)
{
    Vec3 c1, c2;
    closest_points_segments(a.a, a.b, b.a, b.b, &c1, &c2);
    return sphere_sphere(c1, a.radius, c2, b.radius, out_normal, out_depth);
}

bool box_box(const BoxShape& a, const BoxShape& b, Vec3* out_normal,
             float* out_depth)
{
    // Separating Axis Theorem over the 15 candidate axes: 3 face normals
    // each, plus 9 edge-edge cross products. The minimum-overlap axis is the
    // contact normal, which is what makes SAT worth the 15 tests over a
    // cheaper boolean-only test -- the solver needs a direction, not a yes.
    Vec3 axis_a[3] = {quat_rotate(a.rotation, Vec3{1, 0, 0}),
                      quat_rotate(a.rotation, Vec3{0, 1, 0}),
                      quat_rotate(a.rotation, Vec3{0, 0, 1})};
    Vec3 axis_b[3] = {quat_rotate(b.rotation, Vec3{1, 0, 0}),
                      quat_rotate(b.rotation, Vec3{0, 1, 0}),
                      quat_rotate(b.rotation, Vec3{0, 0, 1})};

    Vec3 axes[15];
    uint32_t axis_count = 0;
    for (uint32_t i = 0; i < 3; ++i) {
        axes[axis_count++] = axis_a[i];
    }
    for (uint32_t i = 0; i < 3; ++i) {
        axes[axis_count++] = axis_b[i];
    }
    for (uint32_t i = 0; i < 3; ++i) {
        for (uint32_t j = 0; j < 3; ++j) {
            const Vec3 c = cross(axis_a[i], axis_b[j]);
            // Parallel edges give a zero-length axis, which is already
            // covered by the face normals. Testing it would divide by zero.
            if (length_sq(c) > kEpsilon) {
                axes[axis_count++] = normalize(c);
            }
        }
    }

    const Vec3 delta = sub(a.center, b.center);
    float best_depth = 3.4e38f;
    Vec3 best_axis{0, 1, 0};

    for (uint32_t i = 0; i < axis_count; ++i) {
        const Vec3 axis = axes[i];
        const float ra = fabsf(dot(axis_a[0], axis)) * a.half_extents.x +
                         fabsf(dot(axis_a[1], axis)) * a.half_extents.y +
                         fabsf(dot(axis_a[2], axis)) * a.half_extents.z;
        const float rb = fabsf(dot(axis_b[0], axis)) * b.half_extents.x +
                         fabsf(dot(axis_b[1], axis)) * b.half_extents.y +
                         fabsf(dot(axis_b[2], axis)) * b.half_extents.z;
        const float distance = fabsf(dot(delta, axis));
        const float overlap = ra + rb - distance;
        if (overlap <= 0.0f) {
            return false; // a separating axis: done, they are apart
        }
        if (overlap < best_depth) {
            best_depth = overlap;
            // Orient towards A so the normal obeys the module convention.
            best_axis = dot(delta, axis) < 0.0f ? scale(axis, -1.0f) : axis;
        }
    }
    if (out_normal != nullptr) {
        *out_normal = best_axis;
    }
    if (out_depth != nullptr) {
        *out_depth = best_depth;
    }
    return true;
}

// ---- collider -> shape -----------------------------------------------------

Vec3 sphere_center(const Collider& collider, Vec3 position, Quat rotation)
{
    return add(position, quat_rotate(rotation, collider.center));
}

BoxShape box_shape(const Collider& collider, Vec3 position, Quat rotation)
{
    BoxShape box;
    box.center = add(position, quat_rotate(rotation, collider.center));
    box.half_extents = collider.half_extents;
    box.rotation = rotation;
    return box;
}

CapsuleShape capsule_shape(const Collider& collider, Vec3 position, Quat rotation)
{
    CapsuleShape capsule;
    capsule.radius = collider.half_extents.x;
    // Unity measures height including both hemispherical caps, so the
    // segment is shorter than the height by one diameter. A capsule whose
    // height is less than its diameter degenerates to a sphere rather than
    // inverting, which is also what Unity does.
    const float segment = collider.height - 2.0f * capsule.radius;
    const float half = segment > 0.0f ? segment * 0.5f : 0.0f;

    Vec3 local_axis{0, 1, 0};
    if (collider.axis == CapsuleAxis::X) {
        local_axis = Vec3{1, 0, 0};
    } else if (collider.axis == CapsuleAxis::Z) {
        local_axis = Vec3{0, 0, 1};
    }
    const Vec3 world_center = add(position, quat_rotate(rotation, collider.center));
    const Vec3 world_axis = quat_rotate(rotation, local_axis);
    capsule.a = sub(world_center, scale(world_axis, half));
    capsule.b = add(world_center, scale(world_axis, half));
    return capsule;
}

} // namespace phys
} // namespace ps2ur
