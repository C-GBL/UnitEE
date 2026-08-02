// phys: collision queries, a fixed-step rigid body integrator and a swept
// capsule CharacterController (plan section 9, M11; scope in 7.1).
//
// THIS IS NOT PHYSX AND DOES NOT TRY TO BE. The plan calls it "deliberately
// minimal and deliberately not PhysX-compatible", and that is a promise to
// the user, not an apology: a project ported here will see different contact
// behaviour, different resting jitter and different stacking than the Editor
// does. What it will see is the same API shape, so gameplay code compiles
// and reads the same. Every intentional divergence is listed in
// docs/supported-api.md.
//
// The design in one paragraph. Static geometry is baked offline into a
// world-space BVH over triangles, so the broadphase for the thing that never
// moves costs nothing at runtime and traversal needs no per-query transform.
// Dynamic bodies are primitives only (sphere, box, capsule) -- a moving
// concave mesh is not something a 300 MHz EE should be asked to do, and
// Unity discourages it too. Queries are discrete; the CharacterController
// sweeps, because tunnelling through the floor at speed is the failure mode
// players actually notice.
//
// DETERMINISM (M11 task 5): the step is fixed, the iteration order is array
// order, and there is no double anywhere. That makes a build deterministic
// against itself -- same inputs, same result, every run -- but NOT across
// builds or against the Editor: the EE's floats are not IEEE 754 (section
// 3.1), so a compiler change can move the last bit.
#pragma once

#include "ps2ur/math.h"

#include <cstdint>

namespace ps2ur {
namespace phys {

// ---- limits ---------------------------------------------------------------
//
// Sized for the plan's 6 MB resident-asset budget (15.1) with collision as a
// minority of it. A scene that exceeds one of these is refused at load with
// the table named, not silently truncated.

inline constexpr uint32_t kMaxColliders = 256;
inline constexpr uint32_t kMaxBodies = 64;
inline constexpr uint32_t kMaxCharacters = 8;
inline constexpr uint32_t kMaxLayers = 32;
// Contacts surfaced to managed code in one frame. Overflow is counted and
// reported rather than silently dropped (a lost trigger enter is a bug the
// game will blame on itself).
inline constexpr uint32_t kMaxContacts = 128;
// How many static triangles a single collide-and-slide pass will consider.
inline constexpr uint32_t kMaxTouchedTriangles = 64;

// ---- colliders ------------------------------------------------------------

enum class ColliderKind : uint8_t {
    Sphere = 0,
    Box,     // oriented; the entity's rotation orients it
    Capsule, // oriented; 'axis' picks the local axis before rotation
    Mesh,    // static only, and always the baked BVH
};

// Which local axis a capsule runs along, matching Unity's
// CapsuleCollider.direction (0 = X, 1 = Y, 2 = Z; Unity defaults to Y).
enum class CapsuleAxis : uint8_t { X = 0, Y = 1, Z = 2 };

// One collider component. 'entity' is the scene entity it rides on; a
// static collider has entity < 0 and is already in world space.
//
// Unity semantics preserved deliberately:
//   - is_trigger colliders never resolve contact, only report it.
//   - A collider with no Rigidbody is STATIC to the solver even if its
//     entity moves; Unity says the same and warns about it.
struct Collider {
    ColliderKind kind = ColliderKind::Sphere;
    CapsuleAxis axis = CapsuleAxis::Y;
    bool is_trigger = false;
    bool enabled = true;
    uint8_t layer = 0;
    int32_t entity = -1;
    Vec3 center{0, 0, 0};       // local offset from the entity origin
    Vec3 half_extents{0, 0, 0}; // Box: half size. Sphere/Capsule: x = radius.
    float height = 0.0f;        // Capsule: total height including both caps
    int32_t body = -1;          // index into the body table, or -1
};

// A capsule reduced to the segment + radius form every test wants. Built
// once per query rather than re-derived inside each triangle test.
struct CapsuleShape {
    Vec3 a{0, 0, 0}; // segment start (centre of the lower cap sphere)
    Vec3 b{0, 0, 0}; // segment end
    float radius = 0.0f;
};

// A box in its own space plus the rotation that orients it. Kept as a
// rotation rather than a 3x3 so the inverse transform is a conjugate.
struct BoxShape {
    Vec3 center{0, 0, 0};
    Vec3 half_extents{0, 0, 0};
    Quat rotation = Quat{0, 0, 0, 1};
};

// ---- baked static geometry (M11 task 1) -----------------------------------
//
// A BVH over world-space triangles, produced offline and consumed with no
// fix-up: nodes and indices are file-relative and the runtime points at the
// container's bytes (zero copy, like every other .p2b section).

// 32 bytes exactly, which is what makes traversal cache-friendly on a
// machine with an 8 KB data cache. Internal nodes store the LEFT child and
// keep the right at left+1, so one index covers both.
struct BvhNode {
    Vec3 bmin;
    uint32_t first;  // leaf: first triangle index. internal: left child.
    Vec3 bmax;
    uint32_t count;  // 0 => internal node
};

// 8 bytes. Vertices are shared and indexed, which is what keeps a collision
// mesh smaller than the render mesh it came from.
struct BvhTriangle {
    uint16_t v0 = 0, v1 = 0, v2 = 0;
    uint8_t layer = 0;
    uint8_t flags = 0;
};

// The baked static world. All pointers are into the .p2b buffer.
struct StaticMesh {
    const BvhNode* nodes = nullptr;
    uint32_t node_count = 0;
    const BvhTriangle* triangles = nullptr;
    uint32_t triangle_count = 0;
    const Vec3* vertices = nullptr;
    uint32_t vertex_count = 0;
};

// ---- queries --------------------------------------------------------------

struct RaycastHit {
    Vec3 point{0, 0, 0};
    Vec3 normal{0, 0, 1};
    float distance = 0.0f;
    int32_t collider = -1; // index into the collider table, or -1 for static
    int32_t triangle = -1; // static hits name the triangle
    uint8_t layer = 0;
};

// A layer mask with every layer set: the default for a query that does not
// care, matching Unity's Physics.DefaultRaycastLayers minus the special
// IgnoreRaycast handling (which this runtime does not have).
inline constexpr uint32_t kAllLayers = 0xFFFFFFFFu;

// ---- rigid bodies ---------------------------------------------------------

// Semi-implicit (symplectic) Euler: velocity first, then position from the
// NEW velocity. Explicit Euler adds energy every step and a bouncing object
// climbs; symplectic does not, for one extra line of nothing.
struct RigidBody {
    int32_t entity = -1;
    Vec3 velocity{0, 0, 0};
    Vec3 angular_velocity{0, 0, 0};
    Vec3 accumulated_force{0, 0, 0};
    float mass = 1.0f;
    float drag = 0.0f;
    float angular_drag = 0.05f;
    float restitution = 0.0f; // 0 = no bounce, 1 = perfectly elastic
    bool use_gravity = true;
    bool is_kinematic = false;
    bool freeze_rotation = false;
    bool sleeping = false;
    float sleep_timer = 0.0f;
};

// ---- character controller -------------------------------------------------

// Collide-and-slide over a swept capsule. This, not the rigid body, is what
// most gameplay uses (the plan says so, and it is right).
struct CharacterController {
    int32_t entity = -1;
    float radius = 0.5f;
    float height = 2.0f;    // total, including both caps
    Vec3 center{0, 0, 0};   // local offset
    float slope_limit = 45.0f;  // degrees; steeper counts as a wall
    float step_offset = 0.3f;   // how tall a step may be and still be climbed
    float skin_width = 0.02f;   // keeps the capsule off the surface
    uint8_t layer = 0;
    // Written by move().
    bool grounded = false;
    Vec3 velocity{0, 0, 0}; // the ACHIEVED velocity, after sliding
};

// What a move() call ran into, in Unity's CollisionFlags shape.
inline constexpr uint32_t kCollisionNone = 0;
inline constexpr uint32_t kCollisionSides = 1u << 0;
inline constexpr uint32_t kCollisionAbove = 1u << 1;
inline constexpr uint32_t kCollisionBelow = 1u << 2;

// ---- contacts -------------------------------------------------------------

enum class ContactPhase : uint8_t { Enter = 0, Stay, Exit };

// One collision or trigger event, in the form the managed dispatcher reads.
struct Contact {
    int32_t collider_a = -1;
    int32_t collider_b = -1; // -1 when the other side is static geometry
    ContactPhase phase = ContactPhase::Enter;
    bool is_trigger = false;
    Vec3 point{0, 0, 0};
    Vec3 normal{0, 0, 1};
    float separation = 0.0f;
};

// ---- module ---------------------------------------------------------------

bool init();
void shutdown();
bool initialized();

// Binds the baked static geometry. Null clears it; queries then see only
// dynamic colliders, which is a legitimate state (a scene with no terrain).
void set_static_mesh(const StaticMesh* mesh);
const StaticMesh* static_mesh();

// Gravity in m/s^2. Unity's default is (0, -9.81, 0) and so is this.
void set_gravity(Vec3 g);
Vec3 gravity();

// The fixed timestep. Unity's default is 0.02 s; the plan budgets physics at
// 30 Hz, so this runtime defaults to 1/30 to match the frame rate it can
// actually sustain. Changing it changes simulation behaviour, by design.
void set_fixed_timestep(float dt);
float fixed_timestep();

// ---- layer collision matrix (M11 task 4) ----------------------------------

// Exported from Unity's Physics settings. Symmetric by construction: setting
// (a,b) sets (b,a), because a one-way collision rule is not a thing Unity
// can express and pretending otherwise would produce contacts that appear
// for one object and not the other.
void set_layers_collide(uint32_t a, uint32_t b, bool collide);
bool layers_collide(uint32_t a, uint32_t b);
void reset_layer_matrix(bool collide_all);
// The row for a layer, as a mask -- what a query hands the traversal.
uint32_t layer_mask(uint32_t layer);

// ---- collider table -------------------------------------------------------

int32_t add_collider(const Collider& collider);
uint32_t collider_count();
Collider& collider(uint32_t index);
const Collider& collider_const(uint32_t index);
void clear_colliders();

int32_t add_body(const RigidBody& body);
uint32_t body_count();
RigidBody& body(uint32_t index);
void clear_bodies();

int32_t add_character(const CharacterController& character);
uint32_t character_count();
CharacterController& character(uint32_t index);
void clear_characters();

// ---- the step -------------------------------------------------------------

// Advances one fixed step: integrate bodies, resolve them against static
// geometry and each other, then rebuild the contact list. The caller owns
// the accumulator (the managed FixedUpdate loop already has one).
//
// 'transforms' supplies each collider's world position and rotation, and
// receives the positions the solver produced. Passing the scene's own arrays
// avoids a copy; passing null means every collider is at its stored centre,
// which is what the host tests do.
void step(Vec3* positions, Quat* rotations, uint32_t count);

// ---- world queries --------------------------------------------------------
//
// All of these are DISCRETE except sweep_capsule: a ray has no thickness and
// an overlap test has no motion. The character controller is what sweeps.

bool raycast(Vec3 origin, Vec3 direction, float max_distance, uint32_t mask,
             RaycastHit* out_hit);

// Sphere of 'radius' swept along the ray. Returns the first touch.
bool spherecast(Vec3 origin, float radius, Vec3 direction, float max_distance,
                uint32_t mask, RaycastHit* out_hit);

// Capsule swept along 'motion'. 'out_fraction' is how far it got, 0..1.
bool sweep_capsule(const CapsuleShape& capsule, Vec3 motion, uint32_t mask,
                   float* out_fraction, Vec3* out_normal, int32_t* out_triangle);

// Colliders overlapping a sphere. Writes at most 'capacity' indices and
// returns how many were found -- which may exceed capacity, so a caller that
// cares can notice it was truncated.
uint32_t overlap_sphere(Vec3 center, float radius, uint32_t mask,
                        int32_t* out_colliders, uint32_t capacity);

// True if the sphere touches any static triangle. Cheaper than overlap when
// the answer is all that matters.
bool overlaps_static(Vec3 center, float radius, uint32_t mask);

// ---- character movement ---------------------------------------------------

// Moves the character by 'motion', sliding along whatever it hits. Returns
// the CollisionFlags bits. Updates grounded and velocity on the controller.
uint32_t move_character(uint32_t index, Vec3* position, Vec3 motion);

// ---- contacts -------------------------------------------------------------

uint32_t contact_count();
const Contact& contact(uint32_t index);
// Contacts that did not fit this frame. Non-zero means kMaxContacts is too
// small for the scene, and the game is silently missing events.
uint32_t contacts_dropped();

// ---- statistics (for the 4 ms budget, plan 15.3) --------------------------

struct Stats {
    uint32_t bvh_nodes_visited = 0;
    uint32_t triangle_tests = 0;
    uint32_t pair_tests = 0;
    uint32_t steps = 0;
};
const Stats& stats();
void reset_stats();

// ---- intersection primitives ----------------------------------------------
//
// Exposed because they are the part worth unit-testing directly, and because
// gameplay occasionally wants one without a world query.

// Moller-Trumbore. Returns false for a back-facing hit when 'cull' is set.
bool ray_triangle(Vec3 origin, Vec3 direction, Vec3 v0, Vec3 v1, Vec3 v2,
                  bool cull, float* out_t, Vec3* out_normal);

// Slab test. 'inv_dir' is precomputed by the caller because a traversal
// reuses it across every node.
bool ray_aabb(Vec3 origin, Vec3 inv_dir, Vec3 bmin, Vec3 bmax, float max_t,
              float* out_t);

// Closest point on the segment ab to point p.
Vec3 closest_point_on_segment(Vec3 a, Vec3 b, Vec3 p);

// Closest point on triangle v0v1v2 to p (Ericson's voronoi-region method).
Vec3 closest_point_on_triangle(Vec3 p, Vec3 v0, Vec3 v1, Vec3 v2);

// Sphere vs triangle. Writes the contact normal (pointing at the sphere) and
// the penetration depth.
bool sphere_triangle(Vec3 center, float radius, Vec3 v0, Vec3 v1, Vec3 v2,
                     Vec3* out_normal, float* out_depth);

// Capsule vs triangle, same contract.
bool capsule_triangle(const CapsuleShape& capsule, Vec3 v0, Vec3 v1, Vec3 v2,
                      Vec3* out_normal, float* out_depth);

bool sphere_sphere(Vec3 c0, float r0, Vec3 c1, float r1, Vec3* out_normal,
                   float* out_depth);
bool sphere_box(Vec3 center, float radius, const BoxShape& box,
                Vec3* out_normal, float* out_depth);
bool sphere_capsule(Vec3 center, float radius, const CapsuleShape& capsule,
                    Vec3* out_normal, float* out_depth);
bool capsule_capsule(const CapsuleShape& a, const CapsuleShape& b,
                     Vec3* out_normal, float* out_depth);
bool box_box(const BoxShape& a, const BoxShape& b, Vec3* out_normal,
             float* out_depth);

// Closest points between two segments, the kernel under every capsule test.
void closest_points_segments(Vec3 p1, Vec3 q1, Vec3 p2, Vec3 q2, Vec3* out_c1,
                             Vec3* out_c2);

// The world-space shapes a collider resolves to, given its entity transform.
CapsuleShape capsule_shape(const Collider& collider, Vec3 position, Quat rotation);
BoxShape box_shape(const Collider& collider, Vec3 position, Quat rotation);
Vec3 sphere_center(const Collider& collider, Vec3 position, Quat rotation);

} // namespace phys
} // namespace ps2ur
