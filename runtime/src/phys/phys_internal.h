// Shared between the physics translation units. Not a public header: none
// of this is part of the module's contract, and splitting phys.cpp,
// phys_query.cpp, phys_body.cpp and phys_character.cpp is a compile-time
// convenience, not an interface boundary.
#pragma once

#include "ps2ur/phys.h"

namespace ps2ur {
namespace phys {

// Counters for the 4 ms budget (plan 15.3). A plain global rather than an
// accessor: it is incremented in the innermost loop of every traversal, and
// the whole point is that it costs nothing measurable.
extern Stats g_stats;

// Where each collider currently is. The step writes these from the caller's
// transform arrays so a query that runs between steps sees the same world
// the solver did, rather than re-deriving positions from the scene graph.
extern Vec3 g_positions[kMaxColliders];
extern Quat g_rotations[kMaxColliders];

// Static-geometry helpers shared by the queries, the solver and the
// character controller.
bool raycast_static(Vec3 origin, Vec3 direction, float max_distance,
                    uint32_t mask, RaycastHit* out_hit);

// Triangle indices whose bounds overlap the box. Returns the number FOUND,
// which may exceed 'capacity' -- the caller decides whether truncation
// matters.
uint32_t gather_static_triangles(Vec3 bmin, Vec3 bmax, uint32_t mask,
                                 uint32_t* out_indices, uint32_t capacity);

// Single-collider tests, dispatched on kind. Kept here rather than in the
// public header because they take a collider INDEX and so depend on the
// module's tables.
bool raycast_collider(uint32_t index, Vec3 origin, Vec3 direction,
                      float max_distance, float* out_t, Vec3* out_normal);
bool collider_overlaps_sphere(uint32_t index, Vec3 center, float radius);

// Contact between two dynamic colliders, in the module's normal convention
// (pointing from b towards a).
bool collider_pair(uint32_t a, uint32_t b, Vec3* out_normal, float* out_depth,
                   Vec3* out_point);

// Appends to the frame's contact list, deduplicating enter/stay against the
// previous frame. Owned by phys.cpp; called by the solver.
void record_contact(int32_t a, int32_t b, bool is_trigger, Vec3 point,
                    Vec3 normal, float separation);

// ---- the step's phases, in the order step() runs them ----------------------

// Semi-implicit Euler over every non-kinematic body (phys_body.cpp).
void integrate_bodies(float dt);
// Every dynamic collider against the baked static geometry (phys_body.cpp).
void solve_static();
// Positional correction plus an impulse for one dynamic pair (phys_body.cpp).
void resolve_pair(uint32_t a, uint32_t b, Vec3 normal, float depth);
// Sleep bookkeeping, run LAST so it sees post-resolution velocities.
void update_sleep(float dt);

} // namespace phys
} // namespace ps2ur
