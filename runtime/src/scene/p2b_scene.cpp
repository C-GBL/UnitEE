#include "ps2ur/p2b_scene.h"

#include "ps2ur/log.h"

#include <cstring>

namespace ps2ur {
namespace scene {

namespace {

// Bounded little-endian readers over a section. Every read is checked: batch
// and component offsets come from the file and are hostile until proven.
struct View {
    const uint8_t* data;
    uint32_t size;

    bool ok(uint32_t offset, uint32_t bytes) const
    {
        return offset <= size && bytes <= size - offset;
    }
    uint32_t u32(uint32_t offset) const
    {
        const uint8_t* p = data + offset;
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    }
    int32_t i32(uint32_t offset) const { return static_cast<int32_t>(u32(offset)); }
    float f32(uint32_t offset) const
    {
        union {
            uint32_t u;
            float f;
        } c{u32(offset)};
        return c.f;
    }
    uint16_t u16(uint32_t offset) const
    {
        const uint8_t* p = data + offset;
        return static_cast<uint16_t>(p[0] | (p[1] << 8));
    }
};

} // namespace

bool World::load(const io::P2bFile& file)
{
    m_entity_count = 0;
    m_script_count = 0;
    m_mesh_count = 0;
    m_material_count = 0;
    m_camera = Camera{};
    m_light = DirectionalLight{};
    m_error = "";
    for (uint32_t i = 0; i < kMaxEntities; ++i) {
        m_generation[i] = 1;
        m_entities[i] = Entity{};
    }

    // --- Materials ----------------------------------------------------------
    const io::P2bSection* matl = file.find(io::kSectionMaterial);
    if (matl != nullptr) {
        const View v{matl->data, matl->size};
        const uint32_t stride = 4u + 4u + 16u;
        const uint32_t count = matl->size / stride;
        if (count > kMaxMaterials) {
            m_error = "too many materials";
            return false;
        }
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t at = i * stride;
            m_materials[i].kind = v.u32(at + 0);
            m_materials[i].texture_index = v.u32(at + 4);
        }
        m_material_count = count;
    }

    // --- Meshes -------------------------------------------------------------
    const uint32_t mesh_sections = file.count_of(io::kSectionMesh);
    if (mesh_sections > kMaxMeshes) {
        m_error = "too many meshes";
        return false;
    }
    for (uint32_t mi = 0; mi < mesh_sections; ++mi) {
        const io::P2bSection* sec = file.find(io::kSectionMesh, mi);
        const View v{sec->data, sec->size};
        const uint32_t header_bytes = 4u + 4u + 12u + 4u;
        if (!v.ok(0, header_bytes)) {
            m_error = "mesh header truncated";
            return false;
        }
        LoadedMesh& mesh = m_meshes[mi];
        mesh.batch_count = v.u32(0);
        mesh.material_index = v.u32(4);
        mesh.bounds_center = Vec3{v.f32(8), v.f32(12), v.f32(16)};
        mesh.bounds_radius = v.f32(20);
        if (mesh.batch_count == 0 || mesh.batch_count > kMaxBatchesPerMesh) {
            m_error = "bad batch count";
            return false;
        }
        const uint32_t descs_at = header_bytes;
        if (!v.ok(descs_at, mesh.batch_count * 16u)) {
            m_error = "batch descs truncated";
            return false;
        }
        for (uint32_t b = 0; b < mesh.batch_count; ++b) {
            const uint32_t d = descs_at + b * 16u;
            const uint32_t offset_qw = v.u32(d + 0);
            const uint32_t vert_qw = v.u32(d + 4);
            const uint32_t vcount = v.u32(d + 8);
            const uint32_t vdest = v.u32(d + 12);

            // The blob is [tag][count][verts]: 2 + vert_qw qwords, and it must
            // sit inside the section and be 16-byte aligned in the file.
            const uint32_t byte_off = offset_qw * 16u;
            if (offset_qw > 0x0FFFFFFFu || !v.ok(byte_off, (2u + vert_qw) * 16u)) {
                m_error = "batch blob outside its section";
                return false;
            }
            if (vcount == 0 || vcount % 3u != 0 || vert_qw % vcount != 0) {
                m_error = "batch vertex counts inconsistent";
                return false;
            }
            if (vdest != 10u && vdest != 18u) {
                m_error = "batch vert_dest not a known layout";
                return false;
            }
            const gfx::Qword* blob =
                reinterpret_cast<const gfx::Qword*>(sec->data + byte_off);
            mesh.batches[b] = gfx::BatchBlock{blob, blob + 2, vert_qw, vcount, vdest};
        }
    }
    m_mesh_count = mesh_sections;

    // --- Scene --------------------------------------------------------------
    const io::P2bSection* scn = file.find(io::kSectionScene);
    if (scn == nullptr) {
        m_error = "no SCEN section";
        return false;
    }
    const View v{scn->data, scn->size};
    if (!v.ok(0, 12u)) {
        m_error = "scene header truncated";
        return false;
    }
    const uint32_t entity_count = v.u32(0);
    const uint32_t component_count = v.u32(4);
    if (entity_count == 0 || entity_count > kMaxEntities) {
        m_error = "bad entity count";
        return false;
    }

    const uint32_t entity_stride = 4u + 12u + 16u + 12u + 4u + 2u + 2u + 4u + 4u + 2u + 2u;
    const uint32_t entities_at = 12u;
    if (!v.ok(entities_at, entity_count * entity_stride)) {
        m_error = "entity table truncated";
        return false;
    }
    const uint32_t comps_at = entities_at + entity_count * entity_stride;
    if (!v.ok(comps_at, component_count * 8u)) {
        m_error = "component table truncated";
        return false;
    }

    for (uint32_t i = 0; i < entity_count; ++i) {
        const uint32_t at = entities_at + i * entity_stride;
        Entity& e = m_entities[i];
        e = Entity{};
        e.parent = v.i32(at + 0);
        if (e.parent >= static_cast<int32_t>(i)) {
            // Parent-before-child is what makes the single-pass world update
            // valid; a forward reference would read a stale matrix.
            m_error = "entity parent not before child";
            return false;
        }
        if (e.parent < -1) {
            m_error = "negative parent index";
            return false;
        }
        e.pos = Vec3{v.f32(at + 4), v.f32(at + 8), v.f32(at + 12)};
        e.rot = Quat{v.f32(at + 16), v.f32(at + 20), v.f32(at + 24), v.f32(at + 28)};
        e.scale = Vec3{v.f32(at + 32), v.f32(at + 36), v.f32(at + 40)};
        // Entity byte layout: parent 0, pos 4, rot 16, scale 32, name_hash 44,
        // layer 48, tag 50, flags 52, component_first 56, component_count 60.
        e.name_hash = v.u32(at + 44);
        e.alive = true;
        e.active = true;
        const uint32_t comp_first = v.u32(at + 56);
        const uint32_t comp_n = v.u16(at + 60);

        for (uint32_t c = 0; c < comp_n; ++c) {
            const uint32_t ci = comp_first + c;
            if (ci >= component_count) {
                m_error = "component ref out of range";
                return false;
            }
            const uint32_t cat = comps_at + ci * 8u;
            const uint16_t type = v.u16(cat + 0);
            const uint32_t data_off = v.u32(cat + 4);

            if (type == kComponentMeshRenderer) {
                if (!v.ok(data_off, 8u)) {
                    m_error = "mesh renderer payload truncated";
                    return false;
                }
                const uint32_t mesh_idx = v.u32(data_off + 0);
                const uint32_t mat_idx = v.u32(data_off + 4);
                if (mesh_idx >= m_mesh_count) {
                    m_error = "mesh index out of range";
                    return false;
                }
                if (mat_idx != 0xFFFFFFFFu && mat_idx >= m_material_count) {
                    m_error = "material index out of range";
                    return false;
                }
                e.mesh = static_cast<int32_t>(mesh_idx);
                e.material =
                    mat_idx == 0xFFFFFFFFu ? -1 : static_cast<int32_t>(mat_idx);
            } else if (type == kComponentCamera) {
                if (!v.ok(data_off, 12u)) {
                    m_error = "camera payload truncated";
                    return false;
                }
                m_camera.entity = static_cast<int32_t>(i);
                m_camera.fov = v.f32(data_off + 0);
                m_camera.znear = v.f32(data_off + 4);
                m_camera.zfar = v.f32(data_off + 8);
            } else if (type == kComponentDirectionalLight) {
                if (!v.ok(data_off, 24u)) {
                    m_error = "light payload truncated";
                    return false;
                }
                m_light.entity = static_cast<int32_t>(i);
                m_light.dir = Vec3{v.f32(data_off + 0), v.f32(data_off + 4),
                                   v.f32(data_off + 8)};
                m_light.colour = Vec3{v.f32(data_off + 12), v.f32(data_off + 16),
                                      v.f32(data_off + 20)};
            } else if (type == kComponentScript) {
                // Payload = u32 byte offset of a NUL-terminated type name
                // inside the SCRP section (payloads themselves stay uniform
                // inside SCEN). The NUL must be proven inside the section
                // before the pointer is kept (hostile-input discipline).
                if (!v.ok(data_off, 4u)) {
                    m_error = "script payload truncated";
                    return false;
                }
                const uint32_t name_off = v.u32(data_off);
                const io::P2bSection* scr = file.find(io::kSectionScripts);
                if (scr == nullptr) {
                    m_error = "script component without SCRP section";
                    return false;
                }
                if (name_off >= scr->size) {
                    m_error = "script name offset out of range";
                    return false;
                }
                bool terminated = false;
                for (uint32_t s = name_off; s < scr->size; ++s) {
                    if (scr->data[s] == 0) {
                        terminated = true;
                        break;
                    }
                }
                if (!terminated) {
                    m_error = "script name not NUL-terminated";
                    return false;
                }
                if (m_script_count >= kMaxScripts) {
                    m_error = "too many script components";
                    return false;
                }
                m_scripts[m_script_count].entity = static_cast<int32_t>(i);
                m_scripts[m_script_count].type_name =
                    reinterpret_cast<const char*>(scr->data + name_off);
                ++m_script_count;
            }
            // Unknown component types are skipped, deliberately: old runtimes
            // must tolerate new exporters (forward compatibility).
        }
    }
    m_entity_count = entity_count;
    update_world_matrices();
    return true;
}

void World::update_world_matrices()
{
    // File-ordered scenes resolve in the first pass (parents precede
    // children). Runtime reparenting can create forward references, so keep
    // passing until nothing is pending; the pass count is bounded by the
    // deepest out-of-order chain, and a cycle (impossible via the public
    // API) would leave 'pending' stuck rather than loop forever.
    bool done[kMaxEntities];
    uint32_t pending = 0;
    for (uint32_t i = 0; i < m_entity_count; ++i) {
        done[i] = !m_entities[i].alive;
        if (!done[i]) {
            ++pending;
        }
    }
    while (pending > 0) {
        uint32_t resolved_this_pass = 0;
        for (uint32_t i = 0; i < m_entity_count; ++i) {
            if (done[i]) {
                continue;
            }
            const Entity& e = m_entities[i];
            const int32_t p = e.parent;
            if (p >= 0 && !done[p]) {
                continue;
            }
            const Mat4 local = mat4_trs(e.pos, e.rot, e.scale);
            m_world[i] = p < 0 ? local : mat4_mul(m_world[p], local);
            done[i] = true;
            ++resolved_this_pass;
        }
        if (resolved_this_pass == 0) {
            break; // cycle or dead parent; leave the rest stale, never hang
        }
        pending -= resolved_this_pass;
    }
}

// ---- M7 object model (handles, lifetime) ----------------------------------

int32_t World::handle_of(int32_t index) const
{
    if (index < 0 || index >= static_cast<int32_t>(kMaxEntities) ||
        !m_entities[index].alive) {
        return 0;
    }
    return static_cast<int32_t>(
        (static_cast<uint32_t>(m_generation[index]) << 12) |
        static_cast<uint32_t>(index + 1));
}

int32_t World::resolve(int32_t handle) const
{
    const uint32_t h = static_cast<uint32_t>(handle);
    const int32_t index = static_cast<int32_t>(h & 0xFFFu) - 1;
    if (index < 0 || index >= static_cast<int32_t>(kMaxEntities)) {
        return -1;
    }
    if (!m_entities[index].alive || (h >> 12) != m_generation[index]) {
        return -1;
    }
    return index;
}

int32_t World::create_entity(int32_t parent_index)
{
    int32_t slot = -1;
    for (uint32_t i = 0; i < kMaxEntities; ++i) {
        if (!m_entities[i].alive) {
            slot = static_cast<int32_t>(i);
            break;
        }
    }
    if (slot < 0) {
        return -1;
    }
    Entity& e = m_entities[slot];
    e = Entity{};
    e.alive = true;
    e.active = true;
    e.parent = parent_index;
    if (static_cast<uint32_t>(slot) >= m_entity_count) {
        m_entity_count = static_cast<uint32_t>(slot) + 1;
    }
    m_world[slot] = mat4_identity();
    return slot;
}

void World::destroy_entity(int32_t index)
{
    if (index < 0 || index >= static_cast<int32_t>(kMaxEntities) ||
        !m_entities[index].alive) {
        return;
    }
    // Children first (recursion depth = hierarchy depth, small by design).
    for (uint32_t i = 0; i < m_entity_count; ++i) {
        if (m_entities[i].alive && m_entities[i].parent == index) {
            destroy_entity(static_cast<int32_t>(i));
        }
    }
    m_entities[index].alive = false;
    // Retire every outstanding handle to this slot. 16 generation bits wrap
    // after 65k destroys of one slot; the +1 skip keeps 0 unrepresentable.
    m_generation[index] = static_cast<uint16_t>(m_generation[index] + 1);
    if (m_generation[index] == 0) {
        m_generation[index] = 1;
    }
}

bool World::entity_visible(int32_t index) const
{
    while (index >= 0) {
        const Entity& e = m_entities[index];
        if (!e.alive || !e.active) {
            return false;
        }
        index = e.parent;
    }
    return true;
}

} // namespace scene
} // namespace ps2ur
