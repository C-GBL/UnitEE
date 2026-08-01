// Scene/mesh/material views over a parsed .p2b (plan section 10.4 + M5
// task 5: "SceneLoader that instantiates the entity table").
//
// Still zero-copy where the data allows it: batch payloads are referenced in
// place as BatchBlocks; entity records are unpacked into SoA-ish fixed arrays
// because the runtime updates world matrices every frame and wants them hot.
// Hierarchy is parent-before-child in the file, so world matrices resolve in
// one linear pass (10.4).
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

inline constexpr uint16_t kComponentMeshRenderer = 1;
inline constexpr uint16_t kComponentCamera = 2;
inline constexpr uint16_t kComponentDirectionalLight = 3;

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

class World {
public:
    // Populates from the parsed file. The file's buffer must outlive the
    // world (batches point into it). Returns false with error() set on any
    // structural problem -- offsets are validated against section bounds
    // before use, per the reader obligations in the spec.
    bool load(const io::P2bFile& file);
    const char* error() const { return m_error; }

    // One linear pass, parents strictly before children.
    void update_world_matrices();

    uint32_t entity_count() const { return m_entity_count; }
    const Entity& entity(uint32_t i) const { return m_entities[i]; }
    const Mat4& world_matrix(uint32_t i) const { return m_world[i]; }

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
    uint32_t m_entity_count = 0;

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
