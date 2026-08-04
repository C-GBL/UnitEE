// Physics half of the managed<->native boundary (M11 task 3).
//
// The load-bearing design choice is batching. A frame can produce dozens of
// contacts, and Unity's model is one callback per event per behaviour; doing
// that as one interop call per event would cost more than the solver. So the
// native side accumulates a contact LIST during the step and managed code
// walks it with ps2ur_phys_contact_count + ps2ur_phys_get_contact, then fans
// out to OnCollisionEnter/OnTriggerStay/etc. entirely in managed code -- the
// same shape as the M7 dispatcher, for the same measured reason (a
// runtime_invoke is 87x a direct call).
//
// Colliders are addressed by INDEX, not by handle. They live in a flat table
// the scene owns and never outlive it, so the generation-checked handle
// machinery that entities need would be pure overhead here. Entities are
// still addressed by handle wherever one crosses the boundary.
#include "ps2ur/bridge.h"

#include "ps2ur/p2b_scene.h"
#include "ps2ur/phys.h"

#include "generated_bridge.h"

namespace {

using namespace ps2ur;

// Scratch arrays the step hands to the solver. Physics works on collider
// indices while the scene works on entity indices, so each step gathers the
// transforms of the entities its colliders ride on and writes the solved
// positions back.
Vec3 g_positions[phys::kMaxColliders];
Quat g_rotations[phys::kMaxColliders];

int32_t resolve_entity(int32_t handle)
{
    scene::World* world = bridge::world();
    return world == nullptr ? -1 : world->resolve(handle);
}

} // namespace

// ---- the step --------------------------------------------------------------

extern "C" void ps2ur_phys_step()
{
    scene::World* world = bridge::world();
    const uint32_t count = phys::collider_count();
    if (count == 0) {
        return;
    }

    // Gather. A collider with no entity (baked static geometry) keeps
    // whatever the solver last had, which for static geometry is its baked
    // world position.
    for (uint32_t i = 0; i < count; ++i) {
        const phys::Collider& c = phys::collider_const(i);
        if (world != nullptr && c.entity >= 0 &&
            static_cast<uint32_t>(c.entity) < world->entity_count()) {
            const scene::Entity& e = world->entity(static_cast<uint32_t>(c.entity));
            g_positions[i] = e.pos;
            g_rotations[i] = e.rot;
        }
    }

    phys::step(g_positions, g_rotations, count);

    // Scatter, but only for colliders a body actually moved. Writing back
    // every collider would clobber a transform gameplay set this frame on
    // something physics does not simulate.
    if (world == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const phys::Collider& c = phys::collider_const(i);
        if (c.body < 0 || c.entity < 0 ||
            static_cast<uint32_t>(c.entity) >= world->entity_count()) {
            continue;
        }
        const phys::RigidBody& b = phys::body(static_cast<uint32_t>(c.body));
        if (b.is_kinematic) {
            continue; // gameplay owns a kinematic body's transform
        }
        world->set_local_position(c.entity, g_positions[i]);
        if (!b.freeze_rotation) {
            world->set_local_rotation(c.entity, g_rotations[i]);
        }
    }
}

extern "C" void ps2ur_phys_set_gravity(float x, float y, float z)
{
    phys::set_gravity(Vec3{x, y, z});
}

extern "C" float ps2ur_phys_fixed_timestep() { return phys::fixed_timestep(); }

extern "C" void ps2ur_phys_set_fixed_timestep(float dt)
{
    phys::set_fixed_timestep(dt);
}

extern "C" void ps2ur_phys_set_layers_collide(int32_t a, int32_t b,
                                              int32_t collide)
{
    phys::set_layers_collide(static_cast<uint32_t>(a), static_cast<uint32_t>(b),
                             collide != 0);
}

// ---- queries ---------------------------------------------------------------

namespace {

void fill_hit(P2RaycastHit* out, const phys::RaycastHit& hit)
{
    out->pointX = hit.point.x;
    out->pointY = hit.point.y;
    out->pointZ = hit.point.z;
    out->normalX = hit.normal.x;
    out->normalY = hit.normal.y;
    out->normalZ = hit.normal.z;
    out->distance = hit.distance;
    out->collider = hit.collider;
    out->triangle = hit.triangle;
    out->layer = static_cast<int32_t>(hit.layer);
}

} // namespace

extern "C" int32_t ps2ur_phys_raycast(float originX, float originY,
                                      float originZ, float dirX, float dirY,
                                      float dirZ, float maxDistance,
                                      int32_t mask, P2RaycastHit* hit)
{
    phys::RaycastHit result;
    if (!phys::raycast(Vec3{originX, originY, originZ}, Vec3{dirX, dirY, dirZ},
                       maxDistance, static_cast<uint32_t>(mask), &result)) {
        return 0;
    }
    if (hit != nullptr) {
        fill_hit(hit, result);
    }
    return 1;
}

extern "C" int32_t ps2ur_phys_spherecast(float originX, float originY,
                                         float originZ, float radius,
                                         float dirX, float dirY, float dirZ,
                                         float maxDistance, int32_t mask,
                                         P2RaycastHit* hit)
{
    phys::RaycastHit result;
    if (!phys::spherecast(Vec3{originX, originY, originZ}, radius,
                          Vec3{dirX, dirY, dirZ}, maxDistance,
                          static_cast<uint32_t>(mask), &result)) {
        return 0;
    }
    if (hit != nullptr) {
        fill_hit(hit, result);
    }
    return 1;
}

namespace {

// Scratch for the last overlap query. Sized to the contact budget because a
// query that touches more colliders than a frame can have contacts is
// already past what the runtime is built for. Held natively rather than
// marshalled as an array: no array crosses this boundary (ADR-002).
constexpr uint32_t kOverlapCapacity = phys::kMaxContacts;
int32_t g_overlap[kOverlapCapacity];
uint32_t g_overlap_held = 0;

} // namespace

extern "C" int32_t ps2ur_phys_overlap_sphere(float x, float y, float z,
                                             float radius, int32_t mask)
{
    const uint32_t found =
        phys::overlap_sphere(Vec3{x, y, z}, radius, static_cast<uint32_t>(mask),
                             g_overlap, kOverlapCapacity);
    g_overlap_held = found < kOverlapCapacity ? found : kOverlapCapacity;
    // The FOUND count, not the held count: a caller that asked for more than
    // fits deserves to know it was truncated.
    return static_cast<int32_t>(found);
}

extern "C" int32_t ps2ur_phys_overlap_result(int32_t index)
{
    if (index < 0 || static_cast<uint32_t>(index) >= g_overlap_held) {
        return -1;
    }
    return g_overlap[index];
}

// ---- contacts --------------------------------------------------------------

extern "C" int32_t ps2ur_phys_contact_count()
{
    return static_cast<int32_t>(phys::contact_count());
}

extern "C" int32_t ps2ur_phys_get_contact(int32_t index, P2Contact* contact)
{
    if (index < 0 || static_cast<uint32_t>(index) >= phys::contact_count() ||
        contact == nullptr) {
        return 0;
    }
    const phys::Contact& c = phys::contact(static_cast<uint32_t>(index));
    contact->pointX = c.point.x;
    contact->pointY = c.point.y;
    contact->pointZ = c.point.z;
    contact->normalX = c.normal.x;
    contact->normalY = c.normal.y;
    contact->normalZ = c.normal.z;
    contact->colliderA = c.collider_a;
    contact->colliderB = c.collider_b;
    contact->phase = static_cast<int32_t>(c.phase);
    contact->isTrigger = c.is_trigger ? 1 : 0;
    contact->separation = c.separation;
    return 1;
}

extern "C" int32_t ps2ur_phys_contacts_dropped()
{
    return static_cast<int32_t>(phys::contacts_dropped());
}

// ---- bodies ----------------------------------------------------------------

extern "C" int32_t ps2ur_phys_add_body(int32_t colliderIndex, float mass,
                                       int32_t useGravity, int32_t isKinematic)
{
    if (colliderIndex < 0 ||
        static_cast<uint32_t>(colliderIndex) >= phys::collider_count()) {
        return -1;
    }
    phys::RigidBody body;
    body.mass = mass > 0.0f ? mass : 1.0f;
    body.use_gravity = useGravity != 0;
    body.is_kinematic = isKinematic != 0;
    body.entity = phys::collider_const(static_cast<uint32_t>(colliderIndex)).entity;
    const int32_t index = phys::add_body(body);
    if (index >= 0) {
        phys::collider(static_cast<uint32_t>(colliderIndex)).body = index;
    }
    return index;
}

extern "C" void ps2ur_phys_body_set_velocity(int32_t body, float x, float y,
                                             float z)
{
    if (body >= 0 && static_cast<uint32_t>(body) < phys::body_count()) {
        phys::RigidBody& b = phys::body(static_cast<uint32_t>(body));
        b.velocity = Vec3{x, y, z};
        // Setting a velocity is gameplay saying the body is moving, so it
        // must not stay asleep and ignore it.
        b.sleeping = false;
        b.sleep_timer = 0.0f;
    }
}

extern "C" void ps2ur_phys_body_get_velocity(int32_t body, P2Vec3* value)
{
    if (value == nullptr) {
        return;
    }
    if (body < 0 || static_cast<uint32_t>(body) >= phys::body_count()) {
        *value = P2Vec3{0.0f, 0.0f, 0.0f};
        return;
    }
    const Vec3 v = phys::body(static_cast<uint32_t>(body)).velocity;
    *value = P2Vec3{v.x, v.y, v.z};
}

extern "C" void ps2ur_phys_body_add_force(int32_t body, float x, float y,
                                          float z)
{
    if (body >= 0 && static_cast<uint32_t>(body) < phys::body_count()) {
        phys::RigidBody& b = phys::body(static_cast<uint32_t>(body));
        b.accumulated_force = add(b.accumulated_force, Vec3{x, y, z});
        b.sleeping = false;
        b.sleep_timer = 0.0f;
    }
}

extern "C" void ps2ur_phys_body_set_damping(int32_t body, float linearDamping,
                                            float angularDamping)
{
    if (body >= 0 && static_cast<uint32_t>(body) < phys::body_count()) {
        phys::RigidBody& b = phys::body(static_cast<uint32_t>(body));
        b.drag = linearDamping > 0.0f ? linearDamping : 0.0f;
        b.angular_drag = angularDamping > 0.0f ? angularDamping : 0.0f;
    }
}

extern "C" void ps2ur_phys_body_set_flags(int32_t body, int32_t useGravity,
                                          int32_t isKinematic,
                                          int32_t freezeRotation)
{
    if (body < 0 || static_cast<uint32_t>(body) >= phys::body_count()) {
        return;
    }
    phys::RigidBody& b = phys::body(static_cast<uint32_t>(body));
    b.use_gravity = useGravity != 0;
    b.freeze_rotation = freezeRotation != 0;
    if (b.is_kinematic && isKinematic == 0) {
        // Leaving kinematic mode: the integrator has been clearing this
        // body's force accumulator and never touching its velocity, so it
        // starts from rest rather than resuming whatever it held before.
        b.velocity = Vec3{0, 0, 0};
        b.angular_velocity = Vec3{0, 0, 0};
        b.sleeping = false;
        b.sleep_timer = 0.0f;
    }
    b.is_kinematic = isKinematic != 0;
}

extern "C" int32_t ps2ur_phys_collider_count()
{
    return static_cast<int32_t>(phys::collider_count());
}

extern "C" int32_t ps2ur_phys_collider_entity(int32_t index)
{
    scene::World* world = bridge::world();
    if (world == nullptr || index < 0 ||
        static_cast<uint32_t>(index) >= phys::collider_count()) {
        return 0;
    }
    const int32_t entity = phys::collider_const(static_cast<uint32_t>(index)).entity;
    if (entity < 0 || static_cast<uint32_t>(entity) >= world->entity_count()) {
        return 0;
    }
    return world->handle_of(entity);
}

extern "C" int32_t ps2ur_phys_collider_for_entity(int32_t handle)
{
    const int32_t entity = resolve_entity(handle);
    if (entity < 0) {
        return -1;
    }
    for (uint32_t i = 0; i < phys::collider_count(); ++i) {
        if (phys::collider_const(i).entity == entity) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// ---- character controller --------------------------------------------------

extern "C" int32_t ps2ur_phys_add_character(int32_t handle, float radius,
                                            float height, float slopeLimit,
                                            float stepOffset, float centerX,
                                            float centerY, float centerZ)
{
    const int32_t entity = resolve_entity(handle);
    if (entity < 0) {
        return -1;
    }
    phys::CharacterController c;
    c.entity = entity;
    c.radius = radius > 0.0f ? radius : 0.5f;
    c.height = height > 0.0f ? height : 2.0f;
    c.slope_limit = slopeLimit;
    c.step_offset = stepOffset;
    // Unity's CharacterController.center, in the same WORLD units as the
    // dimensions (the shim scales all four by the transform). Without it a
    // feet-origin character's capsule is half underground.
    c.center = Vec3{centerX, centerY, centerZ};
    return phys::add_character(c);
}

extern "C" int32_t ps2ur_phys_move_character(int32_t index, int32_t handle,
                                             float x, float y, float z)
{
    scene::World* world = bridge::world();
    const int32_t entity = resolve_entity(handle);
    if (world == nullptr || entity < 0 || index < 0 ||
        static_cast<uint32_t>(index) >= phys::character_count()) {
        return static_cast<int32_t>(phys::kCollisionNone);
    }
    Vec3 position = world->entity(static_cast<uint32_t>(entity)).pos;
    const uint32_t flags = phys::move_character(static_cast<uint32_t>(index),
                                                &position, Vec3{x, y, z});
    world->set_local_position(entity, position);
    return static_cast<int32_t>(flags);
}

extern "C" int32_t ps2ur_phys_character_grounded(int32_t index)
{
    if (index < 0 || static_cast<uint32_t>(index) >= phys::character_count()) {
        return 0;
    }
    return phys::character(static_cast<uint32_t>(index)).grounded ? 1 : 0;
}
