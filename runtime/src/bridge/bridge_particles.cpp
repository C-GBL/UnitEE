// PS2ParticleSystem across the boundary (M12.5 task 4, ADR-011). The
// simulation lives in the World; these calls address a system by its
// entity's handle, the same way every other component crosses.
#include "ps2ur/bridge.h"

#include "ps2ur/p2b_scene.h"

#include "generated_bridge.h"

namespace {

using namespace ps2ur;

int32_t system_of(int32_t entity_handle)
{
    scene::World* world = bridge::world();
    if (world == nullptr) {
        return -1;
    }
    const int32_t entity = world->resolve(entity_handle);
    return entity < 0 ? -1 : world->particle_system_for_entity(entity);
}

} // namespace

extern "C" void ps2ur_particles_play(int32_t entity_handle)
{
    const int32_t s = system_of(entity_handle);
    if (s >= 0) {
        bridge::world()->particle_play(static_cast<uint32_t>(s));
    }
}

extern "C" void ps2ur_particles_stop(int32_t entity_handle)
{
    const int32_t s = system_of(entity_handle);
    if (s >= 0) {
        bridge::world()->particle_stop(static_cast<uint32_t>(s));
    }
}

extern "C" void ps2ur_particles_emit(int32_t entity_handle, int32_t count)
{
    const int32_t s = system_of(entity_handle);
    if (s >= 0 && count > 0) {
        bridge::world()->particle_emit(static_cast<uint32_t>(s),
                                       static_cast<uint32_t>(count));
    }
}

extern "C" int32_t ps2ur_particles_is_playing(int32_t entity_handle)
{
    const int32_t s = system_of(entity_handle);
    return s >= 0 &&
                   bridge::world()->particle_state(static_cast<uint32_t>(s))
                       .playing
               ? 1
               : 0;
}

extern "C" int32_t ps2ur_particles_count(int32_t entity_handle)
{
    const int32_t s = system_of(entity_handle);
    return s < 0 ? 0
                 : static_cast<int32_t>(
                       bridge::world()
                           ->particle_state(static_cast<uint32_t>(s))
                           .count);
}
