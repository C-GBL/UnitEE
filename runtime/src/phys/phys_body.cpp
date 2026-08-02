// Rigid bodies: a semi-implicit Euler integrator and impulse resolution
// against static geometry and other bodies (plan section 9, M11 task 2).
//
// SCOPE, stated up front because the gap between this and PhysX is where
// wrong expectations live. There is:
//   - one contact point per pair, not a manifold;
//   - no friction solver (a tangential damping term stands in for it);
//   - no constraint solver, so no joints and no stable stacking;
//   - a single position-correction pass, not an iterative one.
// A box resting on another box will settle but may creep; three boxes in a
// tower will not stay a tower. That is the honest cost of a 4 ms budget on a
// 300 MHz in-order CPU, and the plan asked for exactly this ("a simple
// integrator", "a low iteration count"). Gameplay that needs reliable
// stacking should use the character controller and kinematic bodies, which
// is what PS2-era games did.
#include "ps2ur/phys.h"

#include "phys_internal.h"

#include <cmath>

namespace ps2ur {
namespace phys {

namespace {

constexpr float kEpsilon = 1e-6f;

// How much of the overlap a single pass removes. Under 1 leaves a little
// penetration on purpose: correcting fully every step makes resting contacts
// jitter as they overshoot and re-collide.
constexpr float kCorrectionRatio = 0.8f;
// Overlap below this is ignored entirely -- the slop that keeps a resting
// body from being nudged every single step.
constexpr float kPenetrationSlop = 0.005f;

// A body must be slower than this, for this long, before it sleeps. Without
// sleeping, every resting body pays full solver cost forever, which on a
// 4 ms budget is the difference between 20 crates and 60.
constexpr float kSleepVelocity = 0.08f;
constexpr float kSleepTime = 0.5f;

float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// The collider that a body drives, or -1. A body with no collider still
// integrates (a camera rig, a projectile with its own logic) but never
// collides.
int32_t collider_of_body(int32_t body_index)
{
    for (uint32_t i = 0; i < collider_count(); ++i) {
        if (collider_const(i).body == body_index) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// Applies an impulse along 'normal' to separate a body from something
// immovable, with restitution and a crude tangential friction.
void apply_static_impulse(RigidBody& b, Vec3 normal)
{
    const float along = dot(b.velocity, normal);
    if (along >= 0.0f) {
        return; // already moving away; an impulse would suck it back in
    }
    const Vec3 normal_velocity = scale(normal, along);
    const Vec3 tangent_velocity = sub(b.velocity, normal_velocity);

    // Reflect the normal component by the restitution, keep the tangential.
    // Low-speed contacts drop restitution entirely: bouncing at 1 cm/s is
    // numerical noise, not physics, and it is what makes a resting ball
    // buzz.
    const float bounce = fabsf(along) > 0.5f ? b.restitution : 0.0f;
    // The "friction" here is a fixed tangential damping, not a Coulomb
    // model: there is no normal force to scale it by without a manifold.
    // Documented as an approximation in supported-api.md.
    constexpr float kTangentialDamping = 0.85f;
    b.velocity = add(scale(normal, -along * bounce),
                     scale(tangent_velocity, kTangentialDamping));
}

} // namespace

// ---- integration -----------------------------------------------------------

void integrate_bodies(float dt)
{
    const Vec3 g = gravity();
    for (uint32_t i = 0; i < body_count(); ++i) {
        RigidBody& b = body(i);
        if (b.is_kinematic) {
            // Kinematic bodies are moved by gameplay, not by the solver, but
            // their forces must still be cleared or they accumulate silently
            // and fire the moment the body stops being kinematic.
            b.accumulated_force = Vec3{0, 0, 0};
            continue;
        }
        const int32_t collider_index = collider_of_body(static_cast<int32_t>(i));
        if (b.sleeping) {
            // A sleeping body still needs its forces cleared, and wakes if
            // something pushed it.
            if (length_sq(b.accumulated_force) > kEpsilon) {
                b.sleeping = false;
                b.sleep_timer = 0.0f;
            } else {
                continue;
            }
        }

        Vec3 acceleration = scale(b.accumulated_force,
                                  b.mass > kEpsilon ? 1.0f / b.mass : 0.0f);
        if (b.use_gravity) {
            acceleration = add(acceleration, g);
        }

        // SEMI-IMPLICIT Euler: velocity from the acceleration first, then
        // position from the NEW velocity. Explicit Euler (position from the
        // old velocity) injects energy every step and a bouncing object
        // climbs; this costs nothing extra and does not.
        b.velocity = add(b.velocity, scale(acceleration, dt));
        // Drag as exponential decay rather than a subtracted force, so it
        // can never reverse the velocity at a large dt.
        if (b.drag > 0.0f) {
            b.velocity = scale(b.velocity, 1.0f / (1.0f + b.drag * dt));
        }
        if (b.freeze_rotation) {
            b.angular_velocity = Vec3{0, 0, 0};
        } else if (b.angular_drag > 0.0f) {
            b.angular_velocity =
                scale(b.angular_velocity, 1.0f / (1.0f + b.angular_drag * dt));
        }

        if (collider_index >= 0) {
            const uint32_t ci = static_cast<uint32_t>(collider_index);
            g_positions[ci] = add(g_positions[ci], scale(b.velocity, dt));
            if (!b.freeze_rotation &&
                length_sq(b.angular_velocity) > kEpsilon) {
                const float speed = length(b.angular_velocity);
                const Quat spin = quat_from_axis_angle(
                    scale(b.angular_velocity, 1.0f / speed), speed * dt);
                g_rotations[ci] = quat_normalize(quat_mul(spin, g_rotations[ci]));
            }
        }
        b.accumulated_force = Vec3{0, 0, 0};
    }
}

// ---- sleeping --------------------------------------------------------------

void update_sleep(float dt)
{
    // Deliberately AFTER contact resolution, not at the end of integration.
    //
    // A body resting on the floor has gravity added to its velocity every
    // step (-0.33 m/s at 30 Hz) and has it removed again by the contact
    // impulse. Sampled just after integration it therefore always looks like
    // it is moving at a third of a metre per second and never sleeps;
    // sampled after resolution it is correctly at rest. The symptom of
    // getting this wrong is that nothing ever sleeps and the solver cost
    // never drops, which is exactly what the 4 ms budget cannot afford.
    for (uint32_t i = 0; i < body_count(); ++i) {
        RigidBody& b = body(i);
        if (b.is_kinematic || b.sleeping) {
            continue;
        }
        if (length_sq(b.velocity) < kSleepVelocity * kSleepVelocity) {
            b.sleep_timer += dt;
            if (b.sleep_timer >= kSleepTime) {
                b.sleeping = true;
                b.velocity = Vec3{0, 0, 0};
                b.angular_velocity = Vec3{0, 0, 0};
            }
        } else {
            b.sleep_timer = 0.0f;
        }
    }
}

// ---- static resolution -----------------------------------------------------

void solve_static()
{
    const StaticMesh* mesh = static_mesh();
    if (mesh == nullptr || mesh->node_count == 0) {
        return;
    }
    for (uint32_t i = 0; i < collider_count(); ++i) {
        Collider& c = collider(i);
        if (!c.enabled || c.body < 0 || c.is_trigger) {
            continue;
        }
        RigidBody& b = body(static_cast<uint32_t>(c.body));
        if (b.is_kinematic || b.sleeping) {
            continue;
        }

        // Bounds generous enough to cover the shape plus the correction it
        // might need this step.
        float reach = c.half_extents.x;
        if (c.kind == ColliderKind::Box) {
            reach = length(c.half_extents);
        } else if (c.kind == ColliderKind::Capsule) {
            reach = c.height * 0.5f;
        }
        const Vec3 center = add(g_positions[i], quat_rotate(g_rotations[i], c.center));
        const Vec3 pad{reach, reach, reach};

        uint32_t candidates[kMaxTouchedTriangles];
        const uint32_t found = gather_static_triangles(
            sub(center, pad), add(center, pad), layer_mask(c.layer), candidates,
            kMaxTouchedTriangles);
        const uint32_t tested =
            found < kMaxTouchedTriangles ? found : kMaxTouchedTriangles;

        // Resolve against each touched triangle in turn. Sequential rather
        // than accumulated: a corner contact wants both pushes applied, and
        // averaging the normals of a wall and a floor points into neither.
        for (uint32_t t = 0; t < tested; ++t) {
            const BvhTriangle& tri = mesh->triangles[candidates[t]];
            const Vec3 v0 = mesh->vertices[tri.v0];
            const Vec3 v1 = mesh->vertices[tri.v1];
            const Vec3 v2 = mesh->vertices[tri.v2];

            Vec3 normal;
            float depth = 0.0f;
            bool touching = false;
            g_stats.triangle_tests++;

            if (c.kind == ColliderKind::Sphere) {
                touching = sphere_triangle(
                    add(g_positions[i], quat_rotate(g_rotations[i], c.center)),
                    c.half_extents.x, v0, v1, v2, &normal, &depth);
            } else if (c.kind == ColliderKind::Capsule) {
                touching = capsule_triangle(
                    capsule_shape(c, g_positions[i], g_rotations[i]), v0, v1, v2,
                    &normal, &depth);
            } else if (c.kind == ColliderKind::Box) {
                // A box against a triangle is approximated by its bounding
                // sphere. Exact box-triangle SAT is 13 more axes per
                // triangle and the acceptance case (a kart on terrain) uses
                // a capsule; this is documented, not hidden.
                touching = sphere_triangle(
                    add(g_positions[i], quat_rotate(g_rotations[i], c.center)),
                    length(c.half_extents), v0, v1, v2, &normal, &depth);
            }
            if (!touching || depth <= kPenetrationSlop) {
                continue;
            }

            g_positions[i] = add(
                g_positions[i],
                scale(normal, (depth - kPenetrationSlop) * kCorrectionRatio));
            apply_static_impulse(b, normal);
            record_contact(static_cast<int32_t>(i), -1, false,
                           add(center, scale(normal, -depth)), normal, depth);
            // NOT reset here. A body resting on the ground resolves a small
            // contact every single step; resetting the sleep timer on it
            // would mean nothing that touches the world can ever sleep.
            // update_sleep() looks at the post-resolution velocity instead,
            // which is the honest measure of "is this thing still moving".
        }
    }
}

// ---- dynamic pair resolution -----------------------------------------------

void resolve_pair(uint32_t a, uint32_t b, Vec3 normal, float depth)
{
    if (depth <= kPenetrationSlop) {
        return;
    }
    Collider& ca = collider(a);
    Collider& cb = collider(b);
    RigidBody* ba = ca.body >= 0 ? &body(static_cast<uint32_t>(ca.body)) : nullptr;
    RigidBody* bb = cb.body >= 0 ? &body(static_cast<uint32_t>(cb.body)) : nullptr;

    // Inverse mass is the whole movability model: a kinematic body, a
    // static collider and an infinitely heavy body are all "inverse mass 0"
    // and the same arithmetic covers all three.
    const float inv_a = (ba != nullptr && !ba->is_kinematic && ba->mass > kEpsilon)
                            ? 1.0f / ba->mass
                            : 0.0f;
    const float inv_b = (bb != nullptr && !bb->is_kinematic && bb->mass > kEpsilon)
                            ? 1.0f / bb->mass
                            : 0.0f;
    const float inv_sum = inv_a + inv_b;
    if (inv_sum < kEpsilon) {
        return; // both immovable: nothing to do but report the contact
    }

    // Positional correction, split by inverse mass so the lighter body moves
    // further -- and so a body against a static one takes the whole push.
    const float correction = (depth - kPenetrationSlop) * kCorrectionRatio;
    g_positions[a] = add(g_positions[a], scale(normal, correction * inv_a / inv_sum));
    g_positions[b] = sub(g_positions[b], scale(normal, correction * inv_b / inv_sum));

    // Impulse along the normal, from the relative velocity.
    const Vec3 va = ba != nullptr ? ba->velocity : Vec3{0, 0, 0};
    const Vec3 vb = bb != nullptr ? bb->velocity : Vec3{0, 0, 0};
    const float along = dot(sub(va, vb), normal);

    // Wake only on a MEANINGFUL approach. Two bodies resting against each
    // other are in contact every step; waking them for that would keep a
    // settled pile awake forever, which is the same trap as the static
    // sleep-timer reset above.
    if (along < -kSleepVelocity) {
        if (ba != nullptr) {
            ba->sleeping = false;
            ba->sleep_timer = 0.0f;
        }
        if (bb != nullptr) {
            bb->sleeping = false;
            bb->sleep_timer = 0.0f;
        }
    }
    if (along >= 0.0f) {
        return; // separating already
    }
    const float restitution =
        clampf((ba != nullptr ? ba->restitution : 0.0f) +
                   (bb != nullptr ? bb->restitution : 0.0f),
               0.0f, 1.0f) *
        0.5f;
    const float bounce = fabsf(along) > 0.5f ? restitution : 0.0f;
    const float magnitude = -(1.0f + bounce) * along / inv_sum;
    const Vec3 impulse = scale(normal, magnitude);

    if (ba != nullptr && inv_a > 0.0f) {
        ba->velocity = add(ba->velocity, scale(impulse, inv_a));
    }
    if (bb != nullptr && inv_b > 0.0f) {
        bb->velocity = sub(bb->velocity, scale(impulse, inv_b));
    }
}

} // namespace phys
} // namespace ps2ur
