// The physics world: tables, the layer matrix, the fixed step and the
// contact list (plan section 9, M11 tasks 2-5).
#include "ps2ur/phys.h"

#include "ps2ur/log.h"
#include "phys_internal.h"

#include <cmath>

namespace ps2ur {
namespace phys {

Stats g_stats;
Vec3 g_positions[kMaxColliders];
Quat g_rotations[kMaxColliders];

namespace {

bool g_initialized = false;

Collider g_colliders[kMaxColliders];
uint32_t g_collider_count = 0;
RigidBody g_bodies[kMaxBodies];
uint32_t g_body_count = 0;
CharacterController g_characters[kMaxCharacters];
uint32_t g_character_count = 0;

const StaticMesh* g_static = nullptr;
Vec3 g_gravity{0.0f, -9.81f, 0.0f};
// 1/30 rather than Unity's 1/50: the plan budgets physics at a 30 Hz fixed
// step (M11 acceptance) and the renderer is vsync-locked to 29.97, so one
// physics step per frame is both the cheapest and the most predictable
// arrangement. A project that wants 50 Hz can ask for it.
float g_fixed_dt = 1.0f / 30.0f;

// Row i is the set of layers layer i collides with. Symmetric by
// construction.
uint32_t g_layer_matrix[kMaxLayers];

Contact g_contacts[kMaxContacts];
uint32_t g_contact_count = 0;
uint32_t g_contacts_dropped = 0;

// Pairs that were touching at the end of the previous step, so enter/stay/
// exit can be told apart. A flat array searched linearly: with 128 pairs max
// a hash table would be slower and much harder to keep deterministic.
struct Pair {
    int32_t a;
    int32_t b;
    bool is_trigger;
};
Pair g_previous[kMaxContacts];
uint32_t g_previous_count = 0;
Pair g_current[kMaxContacts];
uint32_t g_current_count = 0;

constexpr float kEpsilon = 1e-6f;

float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// A pair is identified by its two collider indices in ascending order, so
// (a,b) and (b,a) are the same pair however the loops happen to reach it.
void order_pair(int32_t& a, int32_t& b)
{
    if (a > b) {
        const int32_t t = a;
        a = b;
        b = t;
    }
}

int32_t find_pair(const Pair* pairs, uint32_t count, int32_t a, int32_t b)
{
    for (uint32_t i = 0; i < count; ++i) {
        if (pairs[i].a == a && pairs[i].b == b) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

bool append_contact(int32_t a, int32_t b, ContactPhase phase, bool is_trigger,
                    Vec3 point, Vec3 normal, float separation)
{
    if (g_contact_count >= kMaxContacts) {
        ++g_contacts_dropped;
        return false;
    }
    Contact& c = g_contacts[g_contact_count++];
    c.collider_a = a;
    c.collider_b = b;
    c.phase = phase;
    c.is_trigger = is_trigger;
    c.point = point;
    c.normal = normal;
    c.separation = separation;
    return true;
}

} // namespace

// ---- module ---------------------------------------------------------------

bool init()
{
    if (g_initialized) {
        return true;
    }
    clear_colliders();
    clear_bodies();
    clear_characters();
    g_static = nullptr;
    g_gravity = Vec3{0.0f, -9.81f, 0.0f};
    g_fixed_dt = 1.0f / 30.0f;
    reset_layer_matrix(true);
    g_contact_count = 0;
    g_contacts_dropped = 0;
    g_previous_count = 0;
    g_current_count = 0;
    reset_stats();
    g_initialized = true;
    return true;
}

void shutdown()
{
    g_initialized = false;
    g_static = nullptr;
    clear_colliders();
    clear_bodies();
    clear_characters();
    g_contact_count = 0;
    g_previous_count = 0;
    g_current_count = 0;
}

bool initialized() { return g_initialized; }

void set_static_mesh(const StaticMesh* mesh) { g_static = mesh; }
const StaticMesh* static_mesh() { return g_static; }

void set_gravity(Vec3 g) { g_gravity = g; }
Vec3 gravity() { return g_gravity; }

void set_fixed_timestep(float dt)
{
    // A non-positive step would freeze or reverse time; a huge one makes
    // every sweep useless. Clamped rather than asserted because a build
    // profile is data and data can be wrong.
    g_fixed_dt = clampf(dt, 1.0f / 240.0f, 1.0f / 5.0f);
}
float fixed_timestep() { return g_fixed_dt; }

const Stats& stats() { return g_stats; }
void reset_stats() { g_stats = Stats{}; }

// ---- layer matrix (M11 task 4) --------------------------------------------

void reset_layer_matrix(bool collide_all)
{
    for (uint32_t i = 0; i < kMaxLayers; ++i) {
        g_layer_matrix[i] = collide_all ? 0xFFFFFFFFu : 0u;
    }
}

void set_layers_collide(uint32_t a, uint32_t b, bool collide)
{
    if (a >= kMaxLayers || b >= kMaxLayers) {
        return;
    }
    // Both directions, always. Unity's matrix is symmetric and a one-way
    // rule would make a contact appear for one object and not the other,
    // which no gameplay code is written to expect.
    if (collide) {
        g_layer_matrix[a] |= (1u << b);
        g_layer_matrix[b] |= (1u << a);
    } else {
        g_layer_matrix[a] &= ~(1u << b);
        g_layer_matrix[b] &= ~(1u << a);
    }
}

bool layers_collide(uint32_t a, uint32_t b)
{
    if (a >= kMaxLayers || b >= kMaxLayers) {
        return false;
    }
    return (g_layer_matrix[a] & (1u << b)) != 0u;
}

uint32_t layer_mask(uint32_t layer)
{
    return layer < kMaxLayers ? g_layer_matrix[layer] : 0u;
}

// ---- tables ---------------------------------------------------------------

int32_t add_collider(const Collider& c)
{
    if (g_collider_count >= kMaxColliders) {
        PS2UR_LOG_ERROR("phys: collider table full (%u)",
                        static_cast<unsigned>(kMaxColliders));
        return -1;
    }
    const uint32_t index = g_collider_count++;
    g_colliders[index] = c;
    g_positions[index] = Vec3{0, 0, 0};
    g_rotations[index] = quat_identity();
    return static_cast<int32_t>(index);
}

uint32_t collider_count() { return g_collider_count; }
Collider& collider(uint32_t index) { return g_colliders[index]; }
const Collider& collider_const(uint32_t index) { return g_colliders[index]; }

void clear_colliders()
{
    g_collider_count = 0;
    g_contact_count = 0;
    g_previous_count = 0;
    g_current_count = 0;
}

int32_t add_body(const RigidBody& b)
{
    if (g_body_count >= kMaxBodies) {
        PS2UR_LOG_ERROR("phys: rigid body table full (%u)",
                        static_cast<unsigned>(kMaxBodies));
        return -1;
    }
    g_bodies[g_body_count] = b;
    return static_cast<int32_t>(g_body_count++);
}

uint32_t body_count() { return g_body_count; }
RigidBody& body(uint32_t index) { return g_bodies[index]; }
void clear_bodies() { g_body_count = 0; }

int32_t add_character(const CharacterController& c)
{
    if (g_character_count >= kMaxCharacters) {
        PS2UR_LOG_ERROR("phys: character table full (%u)",
                        static_cast<unsigned>(kMaxCharacters));
        return -1;
    }
    g_characters[g_character_count] = c;
    return static_cast<int32_t>(g_character_count++);
}

uint32_t character_count() { return g_character_count; }
CharacterController& character(uint32_t index) { return g_characters[index]; }
void clear_characters() { g_character_count = 0; }

// ---- per-collider tests ----------------------------------------------------

bool raycast_collider(uint32_t index, Vec3 origin, Vec3 direction,
                      float max_distance, float* out_t, Vec3* out_normal)
{
    const Collider& c = g_colliders[index];
    const Vec3 position = g_positions[index];
    const Quat rotation = g_rotations[index];

    switch (c.kind) {
        case ColliderKind::Sphere: {
            // Ray vs sphere, solved as a quadratic in the ray parameter.
            const Vec3 center = sphere_center(c, position, rotation);
            const float radius = c.half_extents.x;
            const Vec3 m = sub(origin, center);
            const float b = dot(m, direction);
            const float cc = dot(m, m) - radius * radius;
            if (cc > 0.0f && b > 0.0f) {
                return false; // origin outside, pointing away
            }
            const float discriminant = b * b - cc;
            if (discriminant < 0.0f) {
                return false;
            }
            float t = -b - sqrtf(discriminant);
            if (t < 0.0f) {
                t = 0.0f; // origin inside
            }
            if (t > max_distance) {
                return false;
            }
            if (out_t != nullptr) {
                *out_t = t;
            }
            if (out_normal != nullptr) {
                const Vec3 hit = add(origin, scale(direction, t));
                *out_normal = normalize(sub(hit, center));
            }
            return true;
        }
        case ColliderKind::Box: {
            // Into the box's frame, then a slab test: an OBB is an AABB
            // wearing a rotation.
            const BoxShape box = box_shape(c, position, rotation);
            const Quat inv = quat_conjugate(box.rotation);
            const Vec3 local_origin = quat_rotate(inv, sub(origin, box.center));
            const Vec3 local_dir = quat_rotate(inv, direction);
            const Vec3 inv_dir{
                fabsf(local_dir.x) > kEpsilon ? 1.0f / local_dir.x : 1e30f,
                fabsf(local_dir.y) > kEpsilon ? 1.0f / local_dir.y : 1e30f,
                fabsf(local_dir.z) > kEpsilon ? 1.0f / local_dir.z : 1e30f};
            float t = 0.0f;
            if (!ray_aabb(local_origin, inv_dir, scale(box.half_extents, -1.0f),
                          box.half_extents, max_distance, &t)) {
                return false;
            }
            if (out_t != nullptr) {
                *out_t = t;
            }
            if (out_normal != nullptr) {
                // Which face: the local axis the hit point is furthest along
                // relative to that axis's half extent.
                const Vec3 p = add(local_origin, scale(local_dir, t));
                const float ax = fabsf(p.x) / (box.half_extents.x + kEpsilon);
                const float ay = fabsf(p.y) / (box.half_extents.y + kEpsilon);
                const float az = fabsf(p.z) / (box.half_extents.z + kEpsilon);
                Vec3 n{0, 1, 0};
                if (ax >= ay && ax >= az) {
                    n = Vec3{p.x >= 0.0f ? 1.0f : -1.0f, 0, 0};
                } else if (ay >= az) {
                    n = Vec3{0, p.y >= 0.0f ? 1.0f : -1.0f, 0};
                } else {
                    n = Vec3{0, 0, p.z >= 0.0f ? 1.0f : -1.0f};
                }
                *out_normal = quat_rotate(box.rotation, n);
            }
            return true;
        }
        case ColliderKind::Capsule: {
            // No closed form worth the code: march the ray and refine. The
            // step is half the radius, so nothing thinner than the capsule
            // can be stepped over.
            const CapsuleShape capsule = capsule_shape(c, position, rotation);
            const float step_size =
                capsule.radius > kEpsilon ? capsule.radius * 0.5f : 0.05f;
            const uint32_t steps =
                static_cast<uint32_t>(max_distance / step_size) + 1u;
            const uint32_t capped = steps > 128u ? 128u : steps;
            float previous = 0.0f;
            for (uint32_t s = 1; s <= capped; ++s) {
                const float t = max_distance * static_cast<float>(s) /
                                static_cast<float>(capped);
                const Vec3 p = add(origin, scale(direction, t));
                const Vec3 on_axis =
                    closest_point_on_segment(capsule.a, capsule.b, p);
                if (length_sq(sub(p, on_axis)) >
                    capsule.radius * capsule.radius) {
                    previous = t;
                    continue;
                }
                float lo = previous;
                float hi = t;
                for (uint32_t iter = 0; iter < 12; ++iter) {
                    const float mid = (lo + hi) * 0.5f;
                    const Vec3 pm = add(origin, scale(direction, mid));
                    const Vec3 am =
                        closest_point_on_segment(capsule.a, capsule.b, pm);
                    if (length_sq(sub(pm, am)) <=
                        capsule.radius * capsule.radius) {
                        hi = mid;
                    } else {
                        lo = mid;
                    }
                }
                if (out_t != nullptr) {
                    *out_t = hi;
                }
                if (out_normal != nullptr) {
                    const Vec3 ph = add(origin, scale(direction, hi));
                    const Vec3 ah =
                        closest_point_on_segment(capsule.a, capsule.b, ph);
                    *out_normal = normalize(sub(ph, ah));
                }
                return true;
            }
            return false;
        }
        case ColliderKind::Mesh:
        default:
            // Mesh colliders are static and live in the BVH; a mesh collider
            // in the dynamic table is an exporter bug, not a query case.
            return false;
    }
}

bool collider_overlaps_sphere(uint32_t index, Vec3 center, float radius)
{
    const Collider& c = g_colliders[index];
    const Vec3 position = g_positions[index];
    const Quat rotation = g_rotations[index];
    switch (c.kind) {
        case ColliderKind::Sphere:
            return sphere_sphere(center, radius,
                                 sphere_center(c, position, rotation),
                                 c.half_extents.x, nullptr, nullptr);
        case ColliderKind::Box:
            return sphere_box(center, radius, box_shape(c, position, rotation),
                              nullptr, nullptr);
        case ColliderKind::Capsule:
            return sphere_capsule(center, radius,
                                  capsule_shape(c, position, rotation), nullptr,
                                  nullptr);
        case ColliderKind::Mesh:
        default:
            return false;
    }
}

bool collider_pair(uint32_t a, uint32_t b, Vec3* out_normal, float* out_depth,
                   Vec3* out_point)
{
    const Collider& ca = g_colliders[a];
    const Collider& cb = g_colliders[b];
    const Vec3 pa = g_positions[a];
    const Vec3 pb = g_positions[b];
    const Quat ra = g_rotations[a];
    const Quat rb = g_rotations[b];

    bool hit = false;
    Vec3 normal{0, 1, 0};
    float depth = 0.0f;

    // Dispatch on the pair. Sphere-first ordering keeps the table small: the
    // reversed cases just flip the normal.
    if (ca.kind == ColliderKind::Sphere && cb.kind == ColliderKind::Sphere) {
        hit = sphere_sphere(sphere_center(ca, pa, ra), ca.half_extents.x,
                            sphere_center(cb, pb, rb), cb.half_extents.x,
                            &normal, &depth);
    } else if (ca.kind == ColliderKind::Sphere && cb.kind == ColliderKind::Box) {
        hit = sphere_box(sphere_center(ca, pa, ra), ca.half_extents.x,
                         box_shape(cb, pb, rb), &normal, &depth);
    } else if (ca.kind == ColliderKind::Box && cb.kind == ColliderKind::Sphere) {
        hit = sphere_box(sphere_center(cb, pb, rb), cb.half_extents.x,
                         box_shape(ca, pa, ra), &normal, &depth);
        normal = scale(normal, -1.0f);
    } else if (ca.kind == ColliderKind::Sphere &&
               cb.kind == ColliderKind::Capsule) {
        hit = sphere_capsule(sphere_center(ca, pa, ra), ca.half_extents.x,
                             capsule_shape(cb, pb, rb), &normal, &depth);
    } else if (ca.kind == ColliderKind::Capsule &&
               cb.kind == ColliderKind::Sphere) {
        hit = sphere_capsule(sphere_center(cb, pb, rb), cb.half_extents.x,
                             capsule_shape(ca, pa, ra), &normal, &depth);
        normal = scale(normal, -1.0f);
    } else if (ca.kind == ColliderKind::Capsule &&
               cb.kind == ColliderKind::Capsule) {
        hit = capsule_capsule(capsule_shape(ca, pa, ra),
                              capsule_shape(cb, pb, rb), &normal, &depth);
    } else if (ca.kind == ColliderKind::Box && cb.kind == ColliderKind::Box) {
        hit = box_box(box_shape(ca, pa, ra), box_shape(cb, pb, rb), &normal,
                      &depth);
    } else if (ca.kind == ColliderKind::Capsule && cb.kind == ColliderKind::Box) {
        // Approximated by the capsule's closest axis point vs the box.
        // Exact capsule-OBB needs its own SAT variant; this is accurate for
        // the face and edge cases gameplay actually produces, and it is
        // documented as an approximation rather than presented as exact.
        const CapsuleShape capsule = capsule_shape(ca, pa, ra);
        const BoxShape box = box_shape(cb, pb, rb);
        const Vec3 on_axis =
            closest_point_on_segment(capsule.a, capsule.b, box.center);
        hit = sphere_box(on_axis, capsule.radius, box, &normal, &depth);
    } else if (ca.kind == ColliderKind::Box && cb.kind == ColliderKind::Capsule) {
        const CapsuleShape capsule = capsule_shape(cb, pb, rb);
        const BoxShape box = box_shape(ca, pa, ra);
        const Vec3 on_axis =
            closest_point_on_segment(capsule.a, capsule.b, box.center);
        hit = sphere_box(on_axis, capsule.radius, box, &normal, &depth);
        normal = scale(normal, -1.0f);
    }

    if (!hit) {
        return false;
    }
    if (out_normal != nullptr) {
        *out_normal = normal;
    }
    if (out_depth != nullptr) {
        *out_depth = depth;
    }
    if (out_point != nullptr) {
        // Midway along the overlap, which is where a single-point contact
        // solver wants to apply its impulse.
        *out_point = add(pb, scale(normal, depth * 0.5f));
    }
    return true;
}

// ---- contacts (M11 task 3) -------------------------------------------------

void record_contact(int32_t a, int32_t b, bool is_trigger, Vec3 point,
                    Vec3 normal, float separation)
{
    int32_t lo = a;
    int32_t hi = b;
    order_pair(lo, hi);

    // Already recorded this step (two solver passes can reach the same
    // pair): keep the first, which is the one the solver acted on.
    if (find_pair(g_current, g_current_count, lo, hi) >= 0) {
        return;
    }
    if (g_current_count < kMaxContacts) {
        Pair& p = g_current[g_current_count++];
        p.a = lo;
        p.b = hi;
        p.is_trigger = is_trigger;
    }

    const bool was_touching = find_pair(g_previous, g_previous_count, lo, hi) >= 0;
    append_contact(lo, hi, was_touching ? ContactPhase::Stay : ContactPhase::Enter,
                   is_trigger, point, normal, separation);
}

namespace {

// Everything that was touching last step and is not now gets an Exit, in the
// order it was recorded -- which is what makes the sequence deterministic.
void emit_exits()
{
    for (uint32_t i = 0; i < g_previous_count; ++i) {
        const Pair& p = g_previous[i];
        if (find_pair(g_current, g_current_count, p.a, p.b) >= 0) {
            continue;
        }
        append_contact(p.a, p.b, ContactPhase::Exit, p.is_trigger,
                       Vec3{0, 0, 0}, Vec3{0, 1, 0}, 0.0f);
    }
    // This step's touch set becomes next step's history.
    for (uint32_t i = 0; i < g_current_count; ++i) {
        g_previous[i] = g_current[i];
    }
    g_previous_count = g_current_count;
    g_current_count = 0;
}

// Narrowphase over every enabled pair the layer matrix allows. O(n^2) in the
// collider count, which at kMaxColliders = 256 is 32,640 pair tests worst
// case -- too many. In practice the count is far lower and the
// both-static reject below is the real filter; a proper broadphase is the
// first thing to add if the 4 ms budget starts to bite (and stats() is there
// to say when).
void solve_pairs()
{
    for (uint32_t i = 0; i < g_collider_count; ++i) {
        const Collider& a = g_colliders[i];
        if (!a.enabled) {
            continue;
        }
        for (uint32_t j = i + 1; j < g_collider_count; ++j) {
            const Collider& b = g_colliders[j];
            if (!b.enabled) {
                continue;
            }
            // Two colliders that cannot move relative to each other never
            // produce a new contact; skipping them is most of the saving in
            // a scene of mostly-static props.
            if (a.body < 0 && b.body < 0) {
                continue;
            }
            if (!layers_collide(a.layer, b.layer)) {
                continue;
            }
            g_stats.pair_tests++;

            Vec3 normal;
            float depth;
            Vec3 point;
            if (!collider_pair(i, j, &normal, &depth, &point)) {
                continue;
            }
            const bool trigger = a.is_trigger || b.is_trigger;
            record_contact(static_cast<int32_t>(i), static_cast<int32_t>(j),
                           trigger, point, normal, depth);
            if (trigger) {
                continue; // triggers report, never resolve
            }
            resolve_pair(i, j, normal, depth);
        }
    }
}

} // namespace

// ---- the step -------------------------------------------------------------

void step(Vec3* positions, Quat* rotations, uint32_t count)
{
    g_contact_count = 0;
    g_stats.steps++;

    // Pull the caller's transforms in. A collider whose entity is not in the
    // array keeps whatever it had, which is what makes a purely-static
    // collider work with no transform array at all.
    const uint32_t n = count < g_collider_count ? count : g_collider_count;
    if (positions != nullptr) {
        for (uint32_t i = 0; i < n; ++i) {
            g_positions[i] = positions[i];
        }
    }
    if (rotations != nullptr) {
        for (uint32_t i = 0; i < n; ++i) {
            g_rotations[i] = rotations[i];
        }
    }

    integrate_bodies(g_fixed_dt);
    solve_static();
    solve_pairs();
    update_sleep(g_fixed_dt);
    emit_exits();

    // Hand the solved positions back.
    if (positions != nullptr) {
        for (uint32_t i = 0; i < n; ++i) {
            positions[i] = g_positions[i];
        }
    }
    if (rotations != nullptr) {
        for (uint32_t i = 0; i < n; ++i) {
            rotations[i] = g_rotations[i];
        }
    }
}

uint32_t contact_count() { return g_contact_count; }
const Contact& contact(uint32_t index) { return g_contacts[index]; }
uint32_t contacts_dropped() { return g_contacts_dropped; }

} // namespace phys
} // namespace ps2ur
