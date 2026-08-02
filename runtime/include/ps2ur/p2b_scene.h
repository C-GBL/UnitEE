// Scene/mesh/material views over a parsed .p2b (plan section 10.4 + M5
// task 5: "SceneLoader that instantiates the entity table"), extended at M7
// with the runtime object model (plan section 9 M7 task 3): generation-
// checked entity handles, runtime create/destroy/reparent, active flags and
// script component references. This is the native truth the managed shim's
// GameObject/Transform handles point at (ADR-002).
//
// Still zero-copy where the data allows it: batch payloads are referenced in
// place as BatchBlocks; entity records are unpacked into SoA-ish fixed arrays
// because the runtime updates world matrices every frame and wants them hot.
// The p2b file guarantees parent-before-child order, but runtime reparenting
// may break it, so the world-matrix pass resolves dependencies iteratively.
#pragma once

#include "ps2ur/gs_batch.h"
#include "ps2ur/math.h"
#include "ps2ur/p2b.h"

#include <cstdint>

namespace ps2ur {
namespace scene {

inline constexpr uint32_t kMaxEntities = 256;
inline constexpr uint32_t kMaxMeshes = 64;
inline constexpr uint32_t kMaxMaterials = 32;
inline constexpr uint32_t kMaxBatchesPerMesh = 32;
inline constexpr uint32_t kMaxScripts = 64;

inline constexpr uint16_t kComponentMeshRenderer = 1;
inline constexpr uint16_t kComponentCamera = 2;
inline constexpr uint16_t kComponentDirectionalLight = 3;
inline constexpr uint16_t kComponentScript = 4;

inline constexpr uint32_t kMaterialUnlit = 0;
inline constexpr uint32_t kMaterialUnlitTextured = 1;
inline constexpr uint32_t kMaterialVertexLit = 2;

struct LoadedMesh {
    uint32_t material_index = 0;
    uint32_t batch_count = 0;
    gfx::BatchBlock batches[kMaxBatchesPerMesh];
    Vec3 bounds_center{0, 0, 0};
    float bounds_radius = 0;
};

struct LoadedMaterial {
    uint32_t kind = kMaterialUnlit;
    uint32_t texture_index = 0xFFFFFFFFu;
};

struct Entity {
    int32_t parent = -1;
    Vec3 pos{0, 0, 0};
    Quat rot{0, 0, 0, 1};
    Vec3 scale{1, 1, 1};
    int32_t mesh = -1;     // LoadedMesh index, or -1
    int32_t material = -1; // override; -1 = mesh's own
    uint32_t name_hash = 0;
    bool alive = false;
    bool active = true;    // activeSelf; activeInHierarchy = entity_visible()
};

struct Camera {
    int32_t entity = -1;
    float fov = 1.0472f;
    float znear = 0.5f;
    float zfar = 100.0f;
};

struct DirectionalLight {
    int32_t entity = -1;
    Vec3 dir{0, 0, -1};
    Vec3 colour{1, 1, 1};
};

// A MonoBehaviour on an entity: the managed type to instantiate at startup.
// type_name points into the p2b buffer ("Full.Type.Name, AssemblyName").
struct ScriptRef {
    int32_t entity = -1;
    const char* type_name = "";
};

class World {
public:
    // Populates from the parsed file. The file's buffer must outlive the
    // world (batches and script names point into it). Returns false with
    // error() set on any structural problem -- offsets are validated against
    // section bounds before use, per the reader obligations in the spec.
    bool load(const io::P2bFile& file);
    const char* error() const { return m_error; }

    // Resolves parent-before-child in one pass for file-ordered scenes and
    // iterates until fixed point after runtime reparenting. Dead entities
    // are skipped.
    void update_world_matrices();

    uint32_t entity_count() const { return m_entity_count; }
    const Entity& entity(uint32_t i) const { return m_entities[i]; }
    Entity& entity_mut(uint32_t i) { return m_entities[i]; }
    const Mat4& world_matrix(uint32_t i) const { return m_world[i]; }

    // ---- M7 object model: handles + lifetime (ADR-002) -------------------
    //
    // Handle layout: bits 0..11 = index+1 (0 means "no entity" everywhere),
    // bits 12..31 = generation. A destroyed slot's generation advances, so a
    // stale handle resolves to -1 forever instead of aliasing a newcomer.

    int32_t handle_of(int32_t index) const;
    int32_t resolve(int32_t handle) const; // entity index, or -1

    // parent_index -1 creates a root. Returns the new index, or -1 if the
    // table is full. New entities are alive, active, identity TRS.
    int32_t create_entity(int32_t parent_index);

    // Destroys the entity and every descendant; their generations advance.
    void destroy_entity(int32_t index);

    bool entity_visible(int32_t index) const; // active up the whole chain

    uint32_t script_count() const { return m_script_count; }
    const ScriptRef& script(uint32_t i) const { return m_scripts[i]; }

    uint32_t mesh_count() const { return m_mesh_count; }
    const LoadedMesh& mesh(uint32_t i) const { return m_meshes[i]; }
    uint32_t material_count() const { return m_material_count; }
    const LoadedMaterial& material(uint32_t i) const { return m_materials[i]; }

    bool has_camera() const { return m_camera.entity >= 0; }
    const Camera& camera() const { return m_camera; }
    bool has_light() const { return m_light.entity >= 0; }
    const DirectionalLight& light() const { return m_light; }

private:
    Entity m_entities[kMaxEntities];
    Mat4 m_world[kMaxEntities];
    uint16_t m_generation[kMaxEntities] = {}; // bumped to >=1 on first use
    uint32_t m_entity_count = 0;              // high-water mark, not live count

    ScriptRef m_scripts[kMaxScripts];
    uint32_t m_script_count = 0;

    LoadedMesh m_meshes[kMaxMeshes];
    uint32_t m_mesh_count = 0;
    LoadedMaterial m_materials[kMaxMaterials];
    uint32_t m_material_count = 0;

    Camera m_camera;
    DirectionalLight m_light;
    const char* m_error = "";
};

} // namespace scene
} // namespace ps2ur
