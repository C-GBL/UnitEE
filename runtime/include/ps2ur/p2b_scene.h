// Scene/mesh/material views over a parsed .p2b (plan section 10.4 + M5
// task 5), extended at M7 with the runtime object model (generation-checked
// handles, create/destroy/reparent, script components) and at M8 with the
// scene-graph machinery (dirty-tracked world matrices, layers, extended
// cameras, materials-as-data). This is the native truth the managed shim's
// GameObject/Transform handles point at (ADR-002) and the renderer's input.
//
// Still zero-copy where the data allows it: batch payloads are referenced in
// place as BatchBlocks; entity records are unpacked into SoA-ish fixed arrays
// because the runtime updates world matrices every frame and wants them hot.
// The p2b file guarantees parent-before-child order, but runtime reparenting
// may break it, so the world-matrix pass resolves dependencies iteratively.
#pragma once

#include "ps2ur/anim.h"
#include "ps2ur/gs_batch.h"
#include "ps2ur/math.h"
#include "ps2ur/p2b.h"

#include <cstdint>

namespace ps2ur {
namespace scene {

inline constexpr uint32_t kMaxEntities = 640;
inline constexpr uint32_t kMaxMeshes = 96;
inline constexpr uint32_t kMaxMaterials = 32;
inline constexpr uint32_t kMaxBatchesPerMesh = 32;
inline constexpr uint32_t kMaxScripts = 64;
// Skinning (M9). A 1,500-triangle character at 16 triangles per batch (the
// VIF NUM ceiling for 5-qword vertices) is ~94 batches, so skinned meshes
// need a far larger batch budget than rigid ones -- hence separate storage.
// A character imported from a DCC tool arrives as MANY renderers over ONE
// skeleton -- body, hair, each cloth piece, each facial plane -- because that
// is how the material assignment is authored. Unity-chan is 19. Sizing these
// for the count of renderers a scene has (rather than the count of
// characters) is what makes an ordinary imported model loadable.
inline constexpr uint32_t kMaxSkinnedMeshes = 24;
inline constexpr uint32_t kMaxSkinBatches = 256;
inline constexpr uint32_t kMaxSkeletons = 2;
inline constexpr uint32_t kMaxControllers = 2;
inline constexpr uint32_t kMaxSkinnedRenderers = 24;
// Animators are NOT per renderer. An anim::Animator carries three poses, a
// bone-matrix array and four clip cursors -- tens of kilobytes -- and every
// renderer on one character must show the SAME pose, so they share one.
// Bound at load, one per distinct (skeleton, controller) pair.
inline constexpr uint32_t kMaxAnimators = 4;

inline constexpr uint16_t kComponentMeshRenderer = 1;
inline constexpr uint16_t kComponentCamera = 2;
inline constexpr uint16_t kComponentDirectionalLight = 3;
inline constexpr uint16_t kComponentScript = 4;
inline constexpr uint16_t kComponentSkinnedMeshRenderer = 5;
inline constexpr uint16_t kComponentRigidbody = 6;
inline constexpr uint16_t kComponentAnimator = 7;

// A Rigidbody at most per entity, so the table is bounded by kMaxEntities;
// this is the far smaller number a scene realistically simulates, and
// overflowing it is a loud load failure rather than a silent drop.
inline constexpr uint32_t kMaxRigidbodies = 64;

// Material kinds (plan section 7.3). The VALUE is the sort-key field too.
inline constexpr uint32_t kMaterialUnlit = 0;
inline constexpr uint32_t kMaterialUnlitTextured = 1;
inline constexpr uint32_t kMaterialVertexLit = 2;
inline constexpr uint32_t kMaterialLitAlpha = 3;  // transparent pass
inline constexpr uint32_t kMaterialCutout = 4;    // alpha test, Z write on
inline constexpr uint32_t kMaterialAdditive = 5;  // transparent pass
inline constexpr uint32_t kMaterialVertexLitFog = 6; // lit + per-vertex F (M8 task 7)
inline constexpr uint32_t kMaterialSkinned = 7;      // vu_skin palette (M9)

struct LoadedMesh {
    uint32_t material_index = 0;
    uint32_t batch_count = 0;
    gfx::BatchBlock batches[kMaxBatchesPerMesh];
    Vec3 bounds_center{0, 0, 0};
    float bounds_radius = 0;
};

// M8 task 5: the GS state for a material is DATA precomputed at export --
// TEST and ALPHA register values are copied into the command stream, not
// derived from branching logic. ZBUF carries a VRAM pointer only the runtime
// knows, so only the Z-write MASK travels as a flag (recorded deviation in
// docs/formats/p2b-container.md).
struct LoadedMaterial {
    uint32_t kind = kMaterialUnlit;
    uint32_t texture_index = 0xFFFFFFFFu;
    uint64_t gs_test = 0;  // TEST_1 value
    uint64_t gs_alpha = 0; // ALPHA_1 value; meaningful when blend is set
    bool blend = false;    // write ALPHA_1 + PRIM carries ABE (set at export)
    bool zwrite = true;
    bool transparent = false; // render pass selection (back-to-front, no Z)
};

struct Entity {
    int32_t parent = -1;
    Vec3 pos{0, 0, 0};
    Quat rot{0, 0, 0, 1};
    Vec3 scale{1, 1, 1};
    int32_t mesh = -1;     // LoadedMesh index, or -1
    int32_t material = -1; // override; -1 = mesh's own
    uint32_t name_hash = 0;
    uint16_t layer = 0;    // Unity layer index 0..31 (camera culling mask)
    bool alive = false;
    bool active = true;    // activeSelf; activeInHierarchy = entity_visible()
    bool dirty = true;     // local TRS or ancestry changed since last pass
};

struct Camera {
    int32_t entity = -1;
    float fov = 1.0472f;   // vertical, radians (perspective)
    float znear = 0.5f;
    float zfar = 100.0f;
    bool orthographic = false;
    float ortho_size = 5.0f;      // half height, world units (Unity semantics)
    float viewport[4] = {0, 0, 1, 1}; // x, y, w, h in [0,1]
    uint32_t clear_flags = 1;     // 1 = solid colour + depth, 2 = depth only
    uint8_t clear_r = 24, clear_g = 28, clear_b = 44;
    uint32_t layer_mask = 0xFFFFFFFFu;
    bool fog_enabled = false;
    uint8_t fog_r = 128, fog_g = 128, fog_b = 128;
    float fog_near = 10.0f;
    float fog_far = 80.0f;
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

// An Animator on an entity (M12.5). The animator INSTANCE is allocated per
// skinned renderer at load; this records which entity owns the component, so
// GetComponent<Animator>() resolves without going through the renderer --
// Unity's own import puts the Animator on the model root and the renderer on
// a child, so the two are usually different entities.
struct AnimatorRef {
    int32_t entity = -1;
    uint32_t controller = 0;
    uint32_t layers = 1;
    // Pool slot this component resolves to, decided at load: a rig with
    // skinned renderers shares theirs; a rig with none -- a rigid-bound
    // model, meshes parented to bones -- gets its own, which DRIVES ENTITY
    // TRANSFORMS instead of a palette (M12.5).
    int32_t animator = -1;
};

// A Rigidbody on an entity (M11): the component state the managed Rigidbody
// is constructed from at load. The native BODY is not created here -- the
// managed side creates it, because it is the managed Rigidbody that owns the
// body index for the rest of the object's life.
struct RigidbodyRef {
    int32_t entity = -1;
    float mass = 1.0f;
    float linear_damping = 0.0f;
    float angular_damping = 0.05f;
    uint32_t flags = 1u; // bit0 use_gravity, bit1 kinematic, bit2 freeze rotation
};

// A skinned mesh (M9). Batches are zero-copy blobs like rigid meshes, but
// each carries the bone table its vertices' local slots index.
struct LoadedSkinnedMesh {
    uint32_t material_index = 0;
    uint32_t batch_count = 0;
    uint32_t skeleton = 0;
    // Header flags bit0 (M12.5): vertices are 6 qwords with a texcoord and
    // the batch tags emit ST+RGBAQ+XYZ2, so these batches MUST run on the
    // textured program -- the vertex stride is baked into the blob.
    bool textured = false;
    gfx::BatchBlock batches[kMaxSkinBatches];
    uint16_t bone_table[kMaxSkinBatches][anim::kMaxPaletteBones];
    uint8_t bone_count[kMaxSkinBatches];
    Vec3 bounds_center{0, 0, 0};
    float bounds_radius = 0;
};

// A SkinnedMeshRenderer component: the entity it draws on, what it draws,
// and which animator drives it.
struct SkinnedRenderer {
    int32_t entity = -1;
    int32_t mesh = -1;      // index into the skinned-mesh table
    int32_t material = -1;  // override; -1 = the mesh's own
    uint32_t skeleton = 0;
    uint32_t controller = 0;
    // Which CHARACTER this renderer belongs to, as the exporter grouped them
    // (by the Animator component that drives each). Renderers sharing a group
    // share one animator; renderers of different groups animate independently
    // even when they share a skeleton and a controller.
    uint32_t animator_group = 0;
    uint32_t animator = 0;  // index into the world's animator pool
};

class World {
public:
    // Populates from the parsed file. The file's buffer must outlive the
    // world (batches and script names point into it). Returns false with
    // error() set on any structural problem -- offsets are validated against
    // section bounds before use, per the reader obligations in the spec.
    bool load(const io::P2bFile& file);

    // Additive load (M10 task 5): merges a second container into a world
    // that is already running, rebasing its entity, mesh and material
    // indices onto what is already there. Every index inside a .p2b is
    // file-relative precisely so this is possible.
    //
    // Entities, meshes, materials, scripts and the whole animation side
    // (skeletons, clips, controllers, skinned meshes and their renderers)
    // all come across with their indices rebased. What does NOT come across
    // is the camera and the light: the running scene's own camera is what
    // the player is looking through.
    //
    // All-or-nothing: if any table would overflow, nothing is merged.
    bool append(const io::P2bFile& file);

    const char* error() const { return m_error; }

    // Recomputes world matrices for entities whose local TRS or ancestry
    // changed (dirty tracking, M8 task 1); resolves parent-before-child in
    // one pass for file-ordered scenes and iterates to fixed point after
    // runtime reparenting. Dead entities are skipped. Clears dirty flags.
    void update_world_matrices();

    uint32_t entity_count() const { return m_entity_count; }
    const Entity& entity(uint32_t i) const { return m_entities[i]; }
    Entity& entity_mut(uint32_t i) { return m_entities[i]; }
    const Mat4& world_matrix(uint32_t i) const { return m_world[i]; }

    // Mutators used by the bridge: mark the dirty flag so the matrix pass
    // touches only what moved. (entity_mut bypasses tracking; tests only.)
    void set_local_position(int32_t index, Vec3 p);
    void set_local_rotation(int32_t index, Quat q);
    void set_local_scale(int32_t index, Vec3 s);
    void set_parent(int32_t index, int32_t parent_index);

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

    uint32_t rigidbody_count() const { return m_rigidbody_count; }
    const RigidbodyRef& rigidbody(uint32_t i) const { return m_rigidbodies[i]; }

    uint32_t animator_ref_count() const { return m_animator_ref_count; }
    const AnimatorRef& animator_ref(uint32_t i) const { return m_animator_refs[i]; }

    // ---- M9: skinning + animation ---------------------------------------

    uint32_t skinned_mesh_count() const { return m_skinned_mesh_count; }
    const LoadedSkinnedMesh& skinned_mesh(uint32_t i) const
    {
        return m_skinned_meshes[i];
    }

    uint32_t skinned_renderer_count() const { return m_skinned_count; }
    const SkinnedRenderer& skinned_renderer(uint32_t i) const
    {
        return m_skinned[i];
    }

    uint32_t skeleton_count() const { return m_skeleton_count; }
    const anim::Skeleton& skeleton(uint32_t i) const { return m_skeletons[i]; }
    uint32_t clip_count() const { return m_clip_count; }
    const anim::Clip& clip(uint32_t i) const { return m_clips[i]; }
    uint32_t controller_count() const { return m_controller_count; }
    const anim::Controller& controller(uint32_t i) const
    {
        return m_controllers[i];
    }

    // One animator per distinct (skeleton, controller) pair, bound at load,
    // SHARED by every renderer that names it -- the 19 renderers of one
    // imported character are one animator, not 19 that would have to be kept
    // in step. Advancing them is the caller's call (the frame loop runs
    // animation between the managed Update and LateUpdate phases, M9 task 4).
    uint32_t animator_count() const { return m_animator_count; }
    anim::Animator& animator(uint32_t i) { return m_animators[i]; }
    const anim::Animator& animator(uint32_t i) const { return m_animators[i]; }
    void update_animators(float dt);

    // The animator driving an entity, or -1. The bridge resolves handles
    // through this so managed Animator components address the right one.
    int32_t animator_for_entity(int32_t entity_index) const;
    // True when 'index' is 'ancestor' or sits below it. Cycle-guarded.
    bool is_descendant_of(int32_t index, int32_t ancestor) const;
    // Baked state index for a name hash, or -1.
    int32_t state_index(uint32_t controller, uint32_t name_hash) const;

    uint32_t mesh_count() const { return m_mesh_count; }
    const LoadedMesh& mesh(uint32_t i) const { return m_meshes[i]; }
    uint32_t material_count() const { return m_material_count; }
    const LoadedMaterial& material(uint32_t i) const { return m_materials[i]; }

    bool has_camera() const { return m_camera.entity >= 0; }
    const Camera& camera() const { return m_camera; }
    Camera& camera_mut() { return m_camera; }
    bool has_light() const { return m_light.entity >= 0; }
    const DirectionalLight& light() const { return m_light; }

private:
    Entity m_entities[kMaxEntities];
    Mat4 m_world[kMaxEntities];
    uint16_t m_generation[kMaxEntities] = {}; // bumped to >=1 on first use
    uint32_t m_entity_count = 0;              // high-water mark, not live count

    ScriptRef m_scripts[kMaxScripts];
    uint32_t m_script_count = 0;

    RigidbodyRef m_rigidbodies[kMaxRigidbodies];
    uint32_t m_rigidbody_count = 0;

    AnimatorRef m_animator_refs[kMaxAnimators];
    uint32_t m_animator_ref_count = 0;

    LoadedSkinnedMesh m_skinned_meshes[kMaxSkinnedMeshes];
    uint32_t m_skinned_mesh_count = 0;
    SkinnedRenderer m_skinned[kMaxSkinnedRenderers];
    uint32_t m_skinned_count = 0;
    anim::Skeleton m_skeletons[kMaxSkeletons];
    uint32_t m_skeleton_count = 0;
    anim::Clip m_clips[anim::kMaxClips];
    uint32_t m_clip_count = 0;
    anim::Controller m_controllers[kMaxControllers];
    uint32_t m_controller_count = 0;
    anim::Animator m_animators[kMaxAnimators];
    uint32_t m_animator_count = 0;
    // Transform-animation mode (M12.5): slots flagged here write their
    // sampled pose to the entities in m_bone_entity each frame -- how a
    // rigid-bound model (Unity: transform animation) moves. Bones match
    // entities by name hash under the Animator's entity, resolved at load.
    bool m_animator_drives_entities[kMaxAnimators] = {};
    int16_t m_bone_entity[kMaxAnimators][anim::kMaxBones];

    LoadedMesh m_meshes[kMaxMeshes];
    uint32_t m_mesh_count = 0;
    LoadedMaterial m_materials[kMaxMaterials];
    uint32_t m_material_count = 0;

    Camera m_camera;
    DirectionalLight m_light;
    const char* m_error = "";
};

// A World is over a megabyte of fixed-size tables and MUST live in BSS or the
// arena, never on a stack -- an automatic one overflows the default stack and
// crashes before load() is even entered, which is exactly how raising the
// M12.5 skinning limits first showed up (16 tests segfaulting at once).
//
// The bound is a tripwire, not a target: it exists so that growing a table
// makes someone look at the total instead of finding out on hardware.
static_assert(sizeof(World) < 2u * 1024u * 1024u,
              "scene::World has outgrown its budget; check the skinning and "
              "animation table sizes before raising this");

} // namespace scene
} // namespace ps2ur
