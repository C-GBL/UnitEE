// Hand-written implementations of the generated bridge prototypes
// (generated_bridge.h; api-def is runtime/src/bridge/bridge-api.json).
//
// Conventions (ADR-002):
//  - A handle that fails to resolve is NOT an error condition here: the
//    managed side owns Unity's destroyed-object semantics and probes with
//    ps2ur_entity_alive. Getters on a dead handle return identity values;
//    setters are ignored. Nothing faults.
//  - The world pointer is bound by the host program before managed code
//    first runs (bridge::bind_world). Every entry tolerates a missing
//    world the same way it tolerates a dead handle.
#include "ps2ur/bridge.h"

#include "ps2ur/log.h"
#include "ps2ur/p2b_scene.h"

#include "generated_bridge.h"

namespace {

ps2ur::scene::World* g_world = nullptr;

// -1 when the handle (or the world) is gone.
int32_t resolve(int32_t handle)
{
    return g_world == nullptr ? -1 : g_world->resolve(handle);
}

} // namespace

namespace ps2ur {
namespace bridge {

void bind_world(scene::World* world)
{
    g_world = world;
}

scene::World* world()
{
    return g_world;
}

} // namespace bridge
} // namespace ps2ur

// ---- entities --------------------------------------------------------------

extern "C" int32_t ps2ur_entity_create(int32_t parentHandle)
{
    if (g_world == nullptr) {
        return 0;
    }
    const int32_t parent = resolve(parentHandle); // -1 (root) when 0/dead
    const int32_t index = g_world->create_entity(parent);
    return index < 0 ? 0 : g_world->handle_of(index);
}

extern "C" void ps2ur_entity_destroy(int32_t handle)
{
    const int32_t index = resolve(handle);
    if (index >= 0) {
        g_world->destroy_entity(index);
    }
}

extern "C" int32_t ps2ur_entity_alive(int32_t handle)
{
    return resolve(handle) >= 0 ? 1 : 0;
}

extern "C" void ps2ur_entity_set_active(int32_t handle, int32_t active)
{
    const int32_t index = resolve(handle);
    if (index >= 0) {
        g_world->entity_mut(static_cast<uint32_t>(index)).active = active != 0;
    }
}

extern "C" int32_t ps2ur_entity_get_active(int32_t handle)
{
    const int32_t index = resolve(handle);
    return index >= 0 && g_world->entity(static_cast<uint32_t>(index)).active ? 1 : 0;
}

// ---- transform locals ------------------------------------------------------

extern "C" void ps2ur_tf_get_local_position(int32_t handle, P2Vec3* value)
{
    const int32_t index = resolve(handle);
    if (index < 0) {
        *value = P2Vec3{0.0f, 0.0f, 0.0f};
        return;
    }
    const ps2ur::Vec3 p = g_world->entity(static_cast<uint32_t>(index)).pos;
    *value = P2Vec3{p.x, p.y, p.z};
}

extern "C" void ps2ur_tf_set_local_position(int32_t handle, float x, float y, float z)
{
    const int32_t index = resolve(handle);
    if (index >= 0) {
        g_world->set_local_position(index, ps2ur::Vec3{x, y, z});
    }
}

extern "C" void ps2ur_tf_get_local_rotation(int32_t handle, P2Quat* value)
{
    const int32_t index = resolve(handle);
    if (index < 0) {
        *value = P2Quat{0.0f, 0.0f, 0.0f, 1.0f};
        return;
    }
    const ps2ur::Quat q = g_world->entity(static_cast<uint32_t>(index)).rot;
    *value = P2Quat{q.x, q.y, q.z, q.w};
}

extern "C" void ps2ur_tf_set_local_rotation(int32_t handle, float x, float y, float z, float w)
{
    const int32_t index = resolve(handle);
    if (index >= 0) {
        g_world->set_local_rotation(index, ps2ur::Quat{x, y, z, w});
    }
}

extern "C" void ps2ur_tf_get_local_scale(int32_t handle, P2Vec3* value)
{
    const int32_t index = resolve(handle);
    if (index < 0) {
        *value = P2Vec3{1.0f, 1.0f, 1.0f};
        return;
    }
    const ps2ur::Vec3 s = g_world->entity(static_cast<uint32_t>(index)).scale;
    *value = P2Vec3{s.x, s.y, s.z};
}

extern "C" void ps2ur_tf_set_local_scale(int32_t handle, float x, float y, float z)
{
    const int32_t index = resolve(handle);
    if (index >= 0) {
        g_world->set_local_scale(index, ps2ur::Vec3{x, y, z});
    }
}

// ---- hierarchy -------------------------------------------------------------

extern "C" int32_t ps2ur_tf_get_parent(int32_t handle)
{
    const int32_t index = resolve(handle);
    if (index < 0) {
        return 0;
    }
    const int32_t parent = g_world->entity(static_cast<uint32_t>(index)).parent;
    return parent < 0 ? 0 : g_world->handle_of(parent);
}

extern "C" void ps2ur_tf_set_parent(int32_t handle, int32_t parentHandle)
{
    const int32_t index = resolve(handle);
    if (index < 0) {
        return;
    }
    const int32_t parent = resolve(parentHandle); // 0/dead -> -1 -> root
    if (parent == index) {
        return; // self-parenting would cycle; managed side never asks for it
    }
    g_world->set_parent(index, parent);
}

extern "C" int32_t ps2ur_tf_child_count(int32_t handle)
{
    const int32_t index = resolve(handle);
    if (index < 0) {
        return 0;
    }
    int32_t count = 0;
    for (uint32_t i = 0; i < g_world->entity_count(); ++i) {
        if (g_world->entity(i).alive && g_world->entity(i).parent == index) {
            ++count;
        }
    }
    return count;
}

// ---- animation (M9) --------------------------------------------------------

namespace {

// Resolves a managed handle to the animator driving that entity, or null.
ps2ur::anim::Animator* animator_of(int32_t handle)
{
    const int32_t index = resolve(handle);
    if (index < 0) {
        return nullptr;
    }
    const int32_t slot = g_world->animator_for_entity(index);
    return slot < 0 ? nullptr : &g_world->animator(static_cast<uint32_t>(slot));
}

// The controller bound to an entity's animator, for name lookups.
int32_t controller_of(int32_t handle)
{
    const int32_t index = resolve(handle);
    if (index < 0) {
        return -1;
    }
    for (uint32_t i = 0; i < g_world->skinned_renderer_count(); ++i) {
        if (g_world->skinned_renderer(i).entity == index) {
            return static_cast<int32_t>(g_world->skinned_renderer(i).controller);
        }
    }
    return -1;
}

} // namespace

extern "C" void ps2ur_anim_update(float dt)
{
    if (g_world != nullptr) {
        g_world->update_animators(dt);
    }
}

extern "C" int32_t ps2ur_animator_play(int32_t handle, uint32_t stateHash)
{
    ps2ur::anim::Animator* animator = animator_of(handle);
    const int32_t controller = controller_of(handle);
    if (animator == nullptr || controller < 0) {
        return 0;
    }
    const int32_t state =
        g_world->state_index(static_cast<uint32_t>(controller), stateHash);
    if (state < 0) {
        return 0;
    }
    animator->play(static_cast<uint32_t>(state));
    return 1;
}

extern "C" int32_t ps2ur_animator_crossfade(int32_t handle, uint32_t stateHash,
                                            float seconds)
{
    ps2ur::anim::Animator* animator = animator_of(handle);
    const int32_t controller = controller_of(handle);
    if (animator == nullptr || controller < 0) {
        return 0;
    }
    const int32_t state =
        g_world->state_index(static_cast<uint32_t>(controller), stateHash);
    if (state < 0) {
        return 0;
    }
    animator->crossfade(static_cast<uint32_t>(state), seconds);
    return 1;
}

extern "C" void ps2ur_animator_set_trigger(int32_t handle, uint32_t paramHash)
{
    ps2ur::anim::Animator* animator = animator_of(handle);
    if (animator != nullptr) {
        animator->set_trigger(paramHash);
    }
}

extern "C" void ps2ur_animator_set_float(int32_t handle, uint32_t paramHash,
                                         float value)
{
    ps2ur::anim::Animator* animator = animator_of(handle);
    if (animator != nullptr) {
        animator->set_float(paramHash, value);
    }
}

extern "C" int32_t ps2ur_animator_is_blending(int32_t handle)
{
    const ps2ur::anim::Animator* animator = animator_of(handle);
    return animator != nullptr && animator->blending() ? 1 : 0;
}

extern "C" int32_t ps2ur_tf_get_child(int32_t handle, int32_t child_index)
{
    const int32_t index = resolve(handle);
    if (index < 0) {
        return 0;
    }
    int32_t seen = 0;
    for (uint32_t i = 0; i < g_world->entity_count(); ++i) {
        if (g_world->entity(i).alive && g_world->entity(i).parent == index) {
            if (seen == child_index) {
                return g_world->handle_of(static_cast<int32_t>(i));
            }
            ++seen;
        }
    }
    return 0;
}
