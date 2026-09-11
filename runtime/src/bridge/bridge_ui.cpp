// uGUI across the boundary (M12.5 task 5). Elements are addressed by their
// table INDEX -- they live in a flat World-owned table that never reorders,
// so the generation-checked handle machinery would be pure overhead, the
// same reasoning as physics colliders (bridge_phys.cpp).
//
// All interaction (focus, clicks, slider values) is MANAGED code; this file
// is only the mutation surface the retained draw table exposes.
#include "ps2ur/bridge.h"

#include "ps2ur/p2b_scene.h"

#include "generated_bridge.h"

namespace {

using namespace ps2ur;

} // namespace

extern "C" int32_t ps2ur_ui_element_for_entity(int32_t entity_handle)
{
    scene::World* world = bridge::world();
    if (world == nullptr) {
        return -1;
    }
    const int32_t entity = world->resolve(entity_handle);
    return entity < 0 ? -1 : world->ui_element_for_entity(entity);
}

extern "C" uint32_t ps2ur_ui_get_colour(int32_t element)
{
    scene::World* world = bridge::world();
    if (world == nullptr || element < 0 ||
        static_cast<uint32_t>(element) >= world->ui_element_count()) {
        return 0x80FFFFFFu; // opaque white, the uGUI default
    }
    return world->ui_element(static_cast<uint32_t>(element)).colour;
}

extern "C" void ps2ur_ui_set_rect(int32_t element, float x, float y, float w,
                                  float h)
{
    scene::World* world = bridge::world();
    if (world != nullptr && element >= 0) {
        world->ui_set_rect(static_cast<uint32_t>(element), x, y, w, h);
    }
}

extern "C" void ps2ur_ui_set_colour(int32_t element, uint32_t rgba)
{
    scene::World* world = bridge::world();
    if (world != nullptr && element >= 0) {
        world->ui_set_colour(static_cast<uint32_t>(element), rgba);
    }
}

extern "C" void ps2ur_ui_set_text(int32_t element, const char* text)
{
    scene::World* world = bridge::world();
    if (world != nullptr && element >= 0) {
        world->ui_set_text(static_cast<uint32_t>(element), text);
    }
}

extern "C" void ps2ur_ui_set_text_glow(int32_t element, float spread, float intensity,
                                       float dilate)
{
    scene::World* world = bridge::world();
    if (world != nullptr && element >= 0) {
        world->ui_set_text_glow(static_cast<uint32_t>(element), spread, intensity, dilate);
    }
}

extern "C" void ps2ur_ui_set_visible(int32_t element, int32_t visible)
{
    scene::World* world = bridge::world();
    if (world != nullptr && element >= 0) {
        world->ui_set_visible(static_cast<uint32_t>(element), visible != 0);
    }
}
