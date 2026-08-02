// CharacterController: a swept capsule that collides and slides, with step
// and slope handling (plan section 9, M11 task 2 -- "this, not the rigid
// body solver, is what most gameplay actually uses").
//
// The algorithm is collide-and-slide, three passes:
//   1. Sweep the capsule along the desired motion.
//   2. On a hit, move to just before it, project the remaining motion onto
//      the surface plane, and go again.
//   3. Give up after a few passes and stop, rather than looping in a corner.
//
// Two departures from a naive implementation, both of which exist because
// they are what players notice:
//
// STEPS. A 20 cm kerb stops a swept capsule dead, because the capsule's
// lower cap hits a vertical face. Before accepting a blocked horizontal
// move, the controller retries it lifted by step_offset and drops back down;
// if that lands on walkable ground, the step is climbed. This is what makes
// stairs work without ramps under them.
//
// SLOPES. A surface steeper than slope_limit is a wall, not a floor: the
// character does not get grounded on it and gravity is not cancelled, so it
// slides back down instead of standing on a cliff face.
#include "ps2ur/phys.h"

#include "phys_internal.h"

#include <cmath>

namespace ps2ur {
namespace phys {

namespace {

constexpr float kEpsilon = 1e-6f;

// Enough to get around a corner (one slide per wall plus a spare), few
// enough that a pathological crevice cannot eat the frame.
constexpr uint32_t kMaxSlides = 4;

// How far down to probe for the ground each move. Longer than the skin so
// the character stays glued to a floor it is walking down, short enough not
// to snap it onto something it genuinely jumped off.
constexpr float kGroundProbe = 0.05f;

Vec3 safe_normalize(Vec3 v, Vec3 fallback)
{
    const float len_sq = length_sq(v);
    if (len_sq < kEpsilon * kEpsilon) {
        return fallback;
    }
    return scale(v, 1.0f / sqrtf(len_sq));
}

// The component of 'v' that survives sliding along a plane with 'normal'.
Vec3 project_on_plane(Vec3 v, Vec3 normal)
{
    return sub(v, scale(normal, dot(v, normal)));
}

// The capsule used for SWEEPING, which is deliberately a skin width thinner
// than the character's nominal shape.
//
// This is what skin_width is for, and getting it wrong is subtle: a
// character standing on the ground has its capsule exactly touching the
// floor, and "touching" counts as overlapping to capsule_triangle. Sweeping
// with the full-size capsule therefore reports an immediate zero-fraction
// hit on every single move, and the character can never walk anywhere -- it
// is pinned by the floor it is standing on. Shrinking by the skin keeps a
// resting character clear of the surface it rests on.
CapsuleShape shape_at(const CharacterController& c, Vec3 position)
{
    CapsuleShape capsule;
    capsule.radius = c.radius - c.skin_width;
    if (capsule.radius < 0.01f) {
        capsule.radius = c.radius; // a skin wider than the capsule is nonsense
    }
    const float segment = c.height - 2.0f * c.radius;
    const float half = segment > 0.0f ? segment * 0.5f : 0.0f;
    const Vec3 center = add(position, c.center);
    capsule.a = sub(center, Vec3{0, half, 0});
    capsule.b = add(center, Vec3{0, half, 0});
    return capsule;
}

// Is this surface something the character can stand on?
bool is_walkable(const CharacterController& c, Vec3 normal)
{
    // cos of the limit against the up axis. Characters are upright by
    // definition here -- a wall-walking character is not something this
    // controller models, and pretending otherwise would be a lie in the API.
    const float limit = cosf(c.slope_limit * 3.14159265f / 180.0f);
    return normal.y >= limit;
}

// One sweep-and-stop. Returns the position reached and reports what was hit.
Vec3 sweep_once(const CharacterController& c, Vec3 position, Vec3 motion,
                uint32_t mask, bool* out_hit, Vec3* out_normal)
{
    *out_hit = false;
    const float distance = length(motion);
    if (distance < kEpsilon) {
        return position;
    }
    float fraction = 1.0f;
    Vec3 normal{0, 1, 0};
    int32_t triangle = -1;
    if (!sweep_capsule(shape_at(c, position), motion, mask, &fraction, &normal,
                       &triangle)) {
        return add(position, motion);
    }
    *out_hit = true;
    *out_normal = normal;

    // Stop a skin width short of the surface. Landing exactly on it means
    // the next sweep starts already touching, which reports a zero fraction
    // forever and freezes the character against the wall.
    const float backoff = c.skin_width / (distance > kEpsilon ? distance : 1.0f);
    float safe = fraction - backoff;
    if (safe < 0.0f) {
        safe = 0.0f;
    }
    return add(position, scale(motion, safe));
}

} // namespace

uint32_t move_character(uint32_t index, Vec3* position, Vec3 motion)
{
    if (index >= character_count() || position == nullptr) {
        return kCollisionNone;
    }
    CharacterController& c = character(index);
    const uint32_t mask = layer_mask(c.layer);
    const Vec3 start = *position;
    Vec3 current = start;
    Vec3 remaining = motion;
    uint32_t flags = kCollisionNone;
    c.grounded = false;

    for (uint32_t pass = 0; pass < kMaxSlides; ++pass) {
        if (length_sq(remaining) < kEpsilon * kEpsilon) {
            break;
        }
        bool hit = false;
        Vec3 normal{0, 1, 0};
        const Vec3 moved = sweep_once(c, current, remaining, mask, &hit, &normal);
        const Vec3 travelled = sub(moved, current);
        current = moved;

        if (!hit) {
            remaining = Vec3{0, 0, 0};
            break;
        }

        // Classify the surface the way Unity's CollisionFlags does.
        if (normal.y > 0.5f) {
            flags |= kCollisionBelow;
        } else if (normal.y < -0.5f) {
            flags |= kCollisionAbove;
        } else {
            flags |= kCollisionSides;
        }
        if (is_walkable(c, normal) && normal.y > 0.0f) {
            c.grounded = true;
        }

        // What is left of the motion after the part already used.
        remaining = sub(remaining, travelled);

        // A step attempt, but only for a wall: sliding up a ramp is already
        // handled by the projection below, and trying to step onto a ceiling
        // is nonsense.
        const bool blocked_horizontally = fabsf(normal.y) <= 0.5f;
        if (blocked_horizontally && c.step_offset > 0.0f) {
            const Vec3 horizontal{remaining.x, 0.0f, remaining.z};
            if (length_sq(horizontal) > kEpsilon) {
                bool up_hit = false;
                Vec3 up_normal{0, 1, 0};
                // Up, forward, then back down: the three moves that make a
                // step. If any of them is blocked, or the landing is too
                // steep to stand on, the step is refused and the character
                // slides along the wall instead.
                const Vec3 lifted = sweep_once(c, current, Vec3{0, c.step_offset, 0},
                                               mask, &up_hit, &up_normal);
                if (!up_hit) {
                    bool fwd_hit = false;
                    Vec3 fwd_normal{0, 1, 0};
                    const Vec3 forward =
                        sweep_once(c, lifted, horizontal, mask, &fwd_hit, &fwd_normal);
                    if (!fwd_hit) {
                        bool down_hit = false;
                        Vec3 down_normal{0, 1, 0};
                        const Vec3 landed = sweep_once(
                            c, forward, Vec3{0, -(c.step_offset + kGroundProbe), 0},
                            mask, &down_hit, &down_normal);
                        if (down_hit && is_walkable(c, down_normal)) {
                            current = landed;
                            c.grounded = true;
                            remaining = Vec3{0, 0, 0};
                            break;
                        }
                    }
                }
            }
        }

        // Slide: keep the component of the motion that runs along the
        // surface, discard the part going into it.
        remaining = project_on_plane(remaining, normal);
        // A steep face gets its upward component removed too, so the
        // character slides DOWN a cliff rather than being helped up it by
        // the projection.
        if (!is_walkable(c, normal) && normal.y > 0.0f && remaining.y > 0.0f) {
            remaining.y = 0.0f;
        }
    }

    // A downward move that ended without a contact still needs a ground
    // check: walking off the top of a shallow ramp should not un-ground the
    // character for a frame and start a fall.
    if (!c.grounded && motion.y <= 0.0f) {
        bool probe_hit = false;
        Vec3 probe_normal{0, 1, 0};
        const Vec3 probed = sweep_once(c, current, Vec3{0, -kGroundProbe, 0}, mask,
                                       &probe_hit, &probe_normal);
        if (probe_hit && is_walkable(c, probe_normal)) {
            c.grounded = true;
            flags |= kCollisionBelow;
            current = probed;
        }
    }

    *position = current;
    // The ACHIEVED velocity, not the requested one: gameplay reading this
    // after walking into a wall should see zero, which is how Unity's
    // CharacterController.velocity behaves.
    const float dt = fixed_timestep();
    c.velocity = dt > kEpsilon ? scale(sub(current, start), 1.0f / dt)
                               : Vec3{0, 0, 0};
    (void)safe_normalize;
    return flags;
}

} // namespace phys
} // namespace ps2ur
