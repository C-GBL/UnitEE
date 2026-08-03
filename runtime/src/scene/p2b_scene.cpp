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
    m_rigidbody_count = 0;
    m_animator_ref_count = 0;
    m_mesh_count = 0;
    m_material_count = 0;
    // The animation tables must reset too. A World is not always fresh: a
    // non-additive scene change loads into the running one, and append()
    // parses into a reused scratch world. Leaving these set carried the
    // previous scene's characters, skeletons and clips into the next load,
    // which showed up as "additive scene does not fit: skinned renderers"
    // on the THIRD load of a run -- not on the second, which is what made it
    // survive M9 (M10, verify-log).
    m_skinned_mesh_count = 0;
    m_skinned_count = 0;
    m_skeleton_count = 0;
    m_clip_count = 0;
    m_controller_count = 0;
    m_animator_count = 0;
    m_camera = Camera{};
    m_light = DirectionalLight{};
    m_error = "";
    for (uint32_t i = 0; i < kMaxEntities; ++i) {
        m_generation[i] = 1;
        m_entities[i] = Entity{};
    }

    // --- Materials ----------------------------------------------------------
    // MATL v2 (M8 task 5): 48-byte records -- kind u32, texture u32,
    // colour 4xf32 (reserved), TEST_1 u64, ALPHA_1 u64, flags u32
    // (bit0 zwrite, bit1 blend, bit2 transparent), pad u32. Breaking change
    // from the 24-byte v1 record; the container minor version was bumped and
    // the reader refuses ambiguity instead of guessing.
    const io::P2bSection* matl = file.find(io::kSectionMaterial);
    if (matl != nullptr) {
        const View v{matl->data, matl->size};
        const uint32_t stride = 48u;
        if (matl->size % stride != 0u) {
            m_error = "MATL not v2 (48-byte records)";
            return false;
        }
        const uint32_t count = matl->size / stride;
        if (count > kMaxMaterials) {
            m_error = "too many materials";
            return false;
        }
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t at = i * stride;
            LoadedMaterial& m = m_materials[i];
            m.kind = v.u32(at + 0);
            m.texture_index = v.u32(at + 4);
            m.gs_test = static_cast<uint64_t>(v.u32(at + 24)) |
                        (static_cast<uint64_t>(v.u32(at + 28)) << 32);
            m.gs_alpha = static_cast<uint64_t>(v.u32(at + 32)) |
                         (static_cast<uint64_t>(v.u32(at + 36)) << 32);
            const uint32_t flags = v.u32(at + 40);
            m.zwrite = (flags & 1u) != 0u;
            m.blend = (flags & 2u) != 0u;
            m.transparent = (flags & 4u) != 0u;
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

    // --- Skeletons (M9) -----------------------------------------------------
    // Bone record: parent i32, name_hash u32, inverse_bind 16 x f32,
    // rest pos 3 / rot 4 / scale 3 -- 112 bytes.
    {
        const uint32_t sections = file.count_of(io::kSectionSkeleton);
        if (sections > kMaxSkeletons) {
            m_error = "too many skeletons";
            return false;
        }
        for (uint32_t si = 0; si < sections; ++si) {
            const io::P2bSection* sec = file.find(io::kSectionSkeleton, si);
            const View v{sec->data, sec->size};
            if (!v.ok(0, 16u)) {
                m_error = "skeleton header truncated";
                return false;
            }
            const uint32_t bones = v.u32(0);
            if (bones == 0 || bones > anim::kMaxBones) {
                m_error = "bad bone count";
                return false;
            }
            const uint32_t stride = 112u;
            if (!v.ok(16u, bones * stride)) {
                m_error = "skeleton table truncated";
                return false;
            }
            anim::Skeleton& skeleton = m_skeletons[si];
            skeleton.bone_count = bones;
            for (uint32_t b = 0; b < bones; ++b) {
                const uint32_t at = 16u + b * stride;
                anim::Bone& bone = skeleton.bones[b];
                bone.parent = v.i32(at + 0);
                if (bone.parent >= static_cast<int32_t>(b)) {
                    // Same rule as the entity table: parents must precede
                    // children or the single-pass bone update reads stale
                    // matrices.
                    m_error = "bone parent not before child";
                    return false;
                }
                if (bone.parent < -1) {
                    m_error = "negative bone parent";
                    return false;
                }
                bone.name_hash = v.u32(at + 4);
                for (uint32_t i = 0; i < 16; ++i) {
                    bone.inverse_bind.m[i] = v.f32(at + 8u + i * 4u);
                }
                bone.rest_pos = Vec3{v.f32(at + 72), v.f32(at + 76), v.f32(at + 80)};
                bone.rest_rot = Quat{v.f32(at + 84), v.f32(at + 88), v.f32(at + 92),
                                     v.f32(at + 96)};
                bone.rest_scale =
                    Vec3{v.f32(at + 100), v.f32(at + 104), v.f32(at + 108)};
            }
            m_skeleton_count = si + 1;
        }
    }

    // --- Clips (M9) ---------------------------------------------------------
    // Header 32 bytes, then tracks (16 bytes each), then 12-byte keys.
    {
        const uint32_t sections = file.count_of(io::kSectionClip);
        if (sections > anim::kMaxClips) {
            m_error = "too many clips";
            return false;
        }
        for (uint32_t ci = 0; ci < sections; ++ci) {
            const io::P2bSection* sec = file.find(io::kSectionClip, ci);
            const View v{sec->data, sec->size};
            if (!v.ok(0, 32u)) {
                m_error = "clip header truncated";
                return false;
            }
            anim::Clip& clip = m_clips[ci];
            clip.name_hash = v.u32(0);
            clip.duration = v.f32(4);
            clip.track_count = v.u32(8);
            clip.loop = (v.u32(12) & 1u) != 0u;
            clip.key_count = v.u32(16);
            if (clip.duration <= 0.0f || clip.track_count > anim::kMaxTracks) {
                m_error = "bad clip header";
                return false;
            }
            const uint32_t tracks_at = 32u;
            if (!v.ok(tracks_at, clip.track_count * 16u)) {
                m_error = "clip track table truncated";
                return false;
            }
            const uint32_t keys_at = tracks_at + clip.track_count * 16u;
            if (!v.ok(keys_at, clip.key_count * 12u)) {
                m_error = "clip key stream truncated";
                return false;
            }
            for (uint32_t t = 0; t < clip.track_count; ++t) {
                const uint32_t at = tracks_at + t * 16u;
                anim::Track& track = clip.tracks[t];
                track.bone = v.u16(at + 0);
                track.channel = sec->data[at + 2];
                track.key_count = v.u16(at + 4);
                track.key_first = v.u32(at + 8);
                track.quant_scale = v.f32(at + 12);
                if (track.key_count == 0 ||
                    track.key_first + track.key_count > clip.key_count) {
                    m_error = "clip track keys out of range";
                    return false;
                }
            }
            clip.keys = sec->data + keys_at;
            m_clip_count = ci + 1;
        }
    }

    // --- Controllers (M9) ---------------------------------------------------
    {
        const uint32_t sections = file.count_of(io::kSectionController);
        if (sections > kMaxControllers) {
            m_error = "too many controllers";
            return false;
        }
        for (uint32_t ci = 0; ci < sections; ++ci) {
            const io::P2bSection* sec = file.find(io::kSectionController, ci);
            const View v{sec->data, sec->size};
            if (!v.ok(0, 16u)) {
                m_error = "controller header truncated";
                return false;
            }
            anim::Controller& controller = m_controllers[ci];
            controller.state_count = v.u32(0);
            controller.transition_count = v.u32(4);
            controller.param_count = v.u32(8);
            if (controller.state_count > anim::kMaxStates ||
                controller.transition_count > anim::kMaxTransitions ||
                controller.param_count > anim::kMaxParams) {
                m_error = "controller too large";
                return false;
            }
            const uint32_t states_at = 16u;
            const uint32_t transitions_at =
                states_at + controller.state_count * 16u;
            const uint32_t params_at =
                transitions_at + controller.transition_count * 16u;
            if (!v.ok(params_at, controller.param_count * 4u)) {
                m_error = "controller tables truncated";
                return false;
            }
            for (uint32_t s = 0; s < controller.state_count; ++s) {
                const uint32_t at = states_at + s * 16u;
                anim::StateDef& state = controller.states[s];
                state.name_hash = v.u32(at + 0);
                state.clip = v.u16(at + 4);
                state.speed = v.f32(at + 8);
                state.loop = (v.u32(at + 12) & 1u) != 0u;
                if (state.clip >= m_clip_count) {
                    m_error = "controller state clip out of range";
                    return false;
                }
            }
            for (uint32_t t = 0; t < controller.transition_count; ++t) {
                const uint32_t at = transitions_at + t * 16u;
                anim::TransitionDef& transition = controller.transitions[t];
                transition.from = v.u16(at + 0);
                transition.to = v.u16(at + 2);
                transition.duration = v.f32(at + 4);
                transition.condition = sec->data[at + 8];
                transition.param = sec->data[at + 9];
                transition.threshold = v.f32(at + 12);
                if (transition.from >= controller.state_count ||
                    transition.to >= controller.state_count) {
                    m_error = "transition state out of range";
                    return false;
                }
            }
            for (uint32_t p = 0; p < controller.param_count; ++p) {
                controller.param_hash[p] = v.u32(params_at + p * 4u);
            }
            m_controller_count = ci + 1;
        }
    }

    // --- Skinned meshes (M9) ------------------------------------------------
    {
        const uint32_t sections = file.count_of(io::kSectionSkinnedMesh);
        if (sections > kMaxSkinnedMeshes) {
            m_error = "too many skinned meshes";
            return false;
        }
        for (uint32_t mi = 0; mi < sections; ++mi) {
            const io::P2bSection* sec = file.find(io::kSectionSkinnedMesh, mi);
            const View v{sec->data, sec->size};
            const uint32_t header_bytes = 32u;
            if (!v.ok(0, header_bytes)) {
                m_error = "skinned mesh header truncated";
                return false;
            }
            LoadedSkinnedMesh& mesh = m_skinned_meshes[mi];
            mesh.batch_count = v.u32(0);
            mesh.material_index = v.u32(4);
            mesh.bounds_center = Vec3{v.f32(8), v.f32(12), v.f32(16)};
            mesh.bounds_radius = v.f32(20);
            mesh.skeleton = v.u32(24);
            // Offset 28 was header pad, always written 0, until M12.5 made
            // it flags: bit0 = textured (6-qword vertices). Unknown bits are
            // a format from the future; refuse rather than misread it.
            const uint32_t skin_flags = v.u32(28);
            if ((skin_flags & ~1u) != 0u) {
                m_error = "unknown skinned mesh flags";
                return false;
            }
            mesh.textured = (skin_flags & 1u) != 0u;
            const uint32_t vert_stride = mesh.textured ? 6u : 5u;
            if (mesh.batch_count == 0 || mesh.batch_count > kMaxSkinBatches) {
                m_error = "bad skinned batch count";
                return false;
            }
            if (mesh.skeleton >= m_skeleton_count) {
                m_error = "skinned mesh skeleton out of range";
                return false;
            }
            const uint32_t descs_at = header_bytes;
            const uint32_t tables_at = descs_at + mesh.batch_count * 16u;
            if (!v.ok(tables_at, mesh.batch_count * 64u)) {
                m_error = "skinned mesh tables truncated";
                return false;
            }
            const uint32_t bones_in_skeleton =
                m_skeletons[mesh.skeleton].bone_count;

            for (uint32_t b = 0; b < mesh.batch_count; ++b) {
                const uint32_t d = descs_at + b * 16u;
                const uint32_t offset_qw = v.u32(d + 0);
                const uint32_t vert_qw = v.u32(d + 4);
                const uint32_t vcount = v.u32(d + 8);
                const uint32_t vdest = v.u32(d + 12);

                const uint32_t byte_off = offset_qw * 16u;
                if (offset_qw > 0x0FFFFFFFu ||
                    !v.ok(byte_off, (2u + vert_qw) * 16u)) {
                    m_error = "skinned batch blob outside its section";
                    return false;
                }
                if (vcount == 0 || vcount % 3u != 0 ||
                    vert_qw != vcount * vert_stride) {
                    m_error = "skinned batch vertex counts inconsistent";
                    return false;
                }
                if (vert_qw > 255u) {
                    // The VIF NUM field is 8 bits; a larger unpack would
                    // silently truncate on target.
                    m_error = "skinned batch exceeds the VIF unpack limit";
                    return false;
                }
                if (vdest != 114u) {
                    m_error = "skinned batch vert_dest not the palette layout";
                    return false;
                }
                const gfx::Qword* blob =
                    reinterpret_cast<const gfx::Qword*>(sec->data + byte_off);
                mesh.batches[b] =
                    gfx::BatchBlock{blob, blob + 2, vert_qw, vcount, vdest};

                // Bone table: u32 count then 24 u16 slots, 64-byte stride.
                const uint32_t t = tables_at + b * 64u;
                const uint32_t count = v.u32(t + 0);
                if (count == 0 || count > anim::kMaxPaletteBones) {
                    m_error = "skinned batch bone table size";
                    return false;
                }
                for (uint32_t s = 0; s < count; ++s) {
                    mesh.bone_table[b][s] = v.u16(t + 4u + s * 2u);
                }
                mesh.bone_count[b] = static_cast<uint8_t>(count);
                if (!anim::validate_partition(mesh.bone_table[b], count,
                                              bones_in_skeleton, nullptr, 0,
                                              anim::kMaxPaletteBones)) {
                    m_error = "skinned batch bone table invalid";
                    return false;
                }
            }
            m_skinned_mesh_count = mi + 1;
        }
    }

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
        e.layer = v.u16(at + 48);
        e.alive = true;
        e.active = true;
        e.dirty = true;
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
                // 12-byte v1 payload (fov/znear/zfar) or the 64-byte M8
                // payload; anything in between falls back to defaults for
                // the missing tail (old runtimes tolerate new exporters and
                // vice versa).
                if (!v.ok(data_off, 12u)) {
                    m_error = "camera payload truncated";
                    return false;
                }
                m_camera.entity = static_cast<int32_t>(i);
                m_camera.fov = v.f32(data_off + 0);
                m_camera.znear = v.f32(data_off + 4);
                m_camera.zfar = v.f32(data_off + 8);
                if (v.ok(data_off, 64u)) {
                    m_camera.orthographic = v.u32(data_off + 12) != 0u;
                    m_camera.ortho_size = v.f32(data_off + 16);
                    m_camera.viewport[0] = v.f32(data_off + 20);
                    m_camera.viewport[1] = v.f32(data_off + 24);
                    m_camera.viewport[2] = v.f32(data_off + 28);
                    m_camera.viewport[3] = v.f32(data_off + 32);
                    m_camera.clear_flags = v.u32(data_off + 36);
                    const uint32_t cc = v.u32(data_off + 40);
                    m_camera.clear_r = static_cast<uint8_t>(cc & 0xFFu);
                    m_camera.clear_g = static_cast<uint8_t>((cc >> 8) & 0xFFu);
                    m_camera.clear_b = static_cast<uint8_t>((cc >> 16) & 0xFFu);
                    m_camera.layer_mask = v.u32(data_off + 44);
                    m_camera.fog_enabled = v.u32(data_off + 48) != 0u;
                    const uint32_t fc = v.u32(data_off + 52);
                    m_camera.fog_r = static_cast<uint8_t>(fc & 0xFFu);
                    m_camera.fog_g = static_cast<uint8_t>((fc >> 8) & 0xFFu);
                    m_camera.fog_b = static_cast<uint8_t>((fc >> 16) & 0xFFu);
                    m_camera.fog_near = v.f32(data_off + 56);
                    m_camera.fog_far = v.f32(data_off + 60);
                }
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
            } else if (type == kComponentSkinnedMeshRenderer) {
                if (!v.ok(data_off, 16u)) {
                    m_error = "skinned renderer payload truncated";
                    return false;
                }
                if (m_skinned_count >= kMaxSkinnedRenderers) {
                    m_error = "too many skinned renderers";
                    return false;
                }
                const uint32_t mesh_idx = v.u32(data_off + 0);
                const uint32_t mat_idx = v.u32(data_off + 4);
                const uint32_t controller_idx = v.u32(data_off + 12);
                if (mesh_idx >= m_skinned_mesh_count) {
                    m_error = "skinned mesh index out of range";
                    return false;
                }
                if (mat_idx != 0xFFFFFFFFu && mat_idx >= m_material_count) {
                    m_error = "skinned material index out of range";
                    return false;
                }
                if (controller_idx >= m_controller_count) {
                    m_error = "skinned controller index out of range";
                    return false;
                }
                SkinnedRenderer& renderer = m_skinned[m_skinned_count];
                renderer.entity = static_cast<int32_t>(i);
                renderer.mesh = static_cast<int32_t>(mesh_idx);
                renderer.material =
                    mat_idx == 0xFFFFFFFFu ? -1 : static_cast<int32_t>(mat_idx);
                renderer.skeleton = m_skinned_meshes[mesh_idx].skeleton;
                renderer.controller = controller_idx;
                renderer.animator_group = v.u32(data_off + 8);

                // Renderers with the same GROUP are one character and share an
                // animator; different groups are different characters and get
                // their own. The exporter decides, because only it can see the
                // hierarchy: it groups by the Animator component that drives
                // each renderer, exactly as Unity does.
                //
                // Sharing cannot be inferred from the skeleton and controller
                // alone -- three characters of the same rig playing different
                // states have both in common and must NOT share -- and neither
                // can separateness, since one imported character's 19
                // renderers differ in nothing but their mesh.
                const uint32_t group = v.u32(data_off + 8);
                int32_t animator_idx = -1;
                for (uint32_t r = 0; r < m_skinned_count; ++r) {
                    if (m_skinned[r].animator_group == group) {
                        animator_idx = static_cast<int32_t>(m_skinned[r].animator);
                        break;
                    }
                }
                if (animator_idx < 0) {
                    if (m_animator_count >= kMaxAnimators) {
                        m_error = "too many distinct animators";
                        return false;
                    }
                    animator_idx = static_cast<int32_t>(m_animator_count);
                    ++m_animator_count;
                }
                renderer.animator = static_cast<uint32_t>(animator_idx);
                ++m_skinned_count;
            } else if (type == kComponentRigidbody) {
                // 16 bytes: mass, linear damping, angular damping, flags.
                if (!v.ok(data_off, 16u)) {
                    m_error = "rigidbody payload truncated";
                    return false;
                }
                if (m_rigidbody_count >= kMaxRigidbodies) {
                    m_error = "too many rigidbodies";
                    return false;
                }
                RigidbodyRef& rb = m_rigidbodies[m_rigidbody_count];
                rb.entity = static_cast<int32_t>(i);
                rb.mass = v.f32(data_off + 0);
                rb.linear_damping = v.f32(data_off + 4);
                rb.angular_damping = v.f32(data_off + 8);
                rb.flags = v.u32(data_off + 12);
                ++m_rigidbody_count;
            } else if (type == kComponentAnimator) {
                // 8 bytes: controller index and the layer count baked.
                if (!v.ok(data_off, 8u)) {
                    m_error = "animator payload truncated";
                    return false;
                }
                if (m_animator_ref_count >= kMaxAnimators) {
                    m_error = "too many animators";
                    return false;
                }
                AnimatorRef& ar = m_animator_refs[m_animator_ref_count];
                ar.entity = static_cast<int32_t>(i);
                ar.controller = v.u32(data_off + 0);
                ar.layers = v.u32(data_off + 4);
                ++m_animator_ref_count;
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

    // Bind one animator per skinned renderer (M9). They start in their
    // controller's first state; the frame loop drives them.
    for (uint32_t i = 0; i < m_skinned_count; ++i) {
        const SkinnedRenderer& renderer = m_skinned[i];
        m_animators[renderer.animator].bind(&m_skeletons[renderer.skeleton],
                                            &m_controllers[renderer.controller],
                                            m_clips, m_clip_count);
    }

    update_world_matrices();
    return true;
}

bool World::append(const io::P2bFile& file)
{
    // Load the incoming container into a scratch world, then rebase its
    // indices onto ours. Parsing into a second World rather than merging
    // in place means a malformed additive scene cannot corrupt the running
    // one: it fails before anything is copied.
    static World incoming;
    if (!incoming.load(file)) {
        m_error = incoming.error();
        return false;
    }

    const uint32_t mesh_base = m_mesh_count;
    const uint32_t material_base = m_material_count;
    const uint32_t entity_base = m_entity_count;
    const uint32_t script_base = m_script_count;
    const uint32_t rigidbody_base = m_rigidbody_count;
    const uint32_t animator_ref_base = m_animator_ref_count;
    const uint32_t animator_base = m_animator_count;
    const uint32_t skinned_mesh_base = m_skinned_mesh_count;
    const uint32_t skinned_base = m_skinned_count;
    const uint32_t skeleton_base = m_skeleton_count;
    const uint32_t clip_base = m_clip_count;
    const uint32_t controller_base = m_controller_count;

    // Every table has to fit BEFORE anything is copied, or a scene that
    // overflows halfway leaves the running world half-merged. Naming the
    // table that filled up is the difference between a five-minute fix and
    // an afternoon: the caller has to know WHICH budget to raise.
    m_error = "";
    if (entity_base + incoming.m_entity_count > kMaxEntities) {
        m_error = "additive scene does not fit: entities";
    } else if (mesh_base + incoming.m_mesh_count > kMaxMeshes) {
        m_error = "additive scene does not fit: meshes";
    } else if (material_base + incoming.m_material_count > kMaxMaterials) {
        m_error = "additive scene does not fit: materials";
    } else if (script_base + incoming.m_script_count > kMaxScripts) {
        m_error = "additive scene does not fit: scripts";
    } else if (rigidbody_base + incoming.m_rigidbody_count > kMaxRigidbodies) {
        m_error = "additive scene does not fit: rigidbodies";
    } else if (animator_ref_base + incoming.m_animator_ref_count >
               kMaxAnimators) {
        m_error = "additive scene does not fit: animators";
    } else if (animator_base + incoming.m_animator_count > kMaxAnimators) {
        m_error = "additive scene does not fit: animator pool";
    } else if (skinned_mesh_base + incoming.m_skinned_mesh_count >
               kMaxSkinnedMeshes) {
        m_error = "additive scene does not fit: skinned meshes";
    } else if (skinned_base + incoming.m_skinned_count > kMaxSkinnedRenderers) {
        m_error = "additive scene does not fit: skinned renderers";
    } else if (skeleton_base + incoming.m_skeleton_count > kMaxSkeletons) {
        m_error = "additive scene does not fit: skeletons";
    } else if (clip_base + incoming.m_clip_count > anim::kMaxClips) {
        m_error = "additive scene does not fit: clips";
    } else if (controller_base + incoming.m_controller_count > kMaxControllers) {
        m_error = "additive scene does not fit: controllers";
    }
    if (m_error[0] != '\0') {
        return false;
    }

    for (uint32_t i = 0; i < incoming.m_material_count; ++i) {
        m_materials[material_base + i] = incoming.m_materials[i];
    }
    for (uint32_t i = 0; i < incoming.m_mesh_count; ++i) {
        LoadedMesh mesh = incoming.m_meshes[i];
        mesh.material_index += material_base;
        m_meshes[mesh_base + i] = mesh;
    }
    for (uint32_t i = 0; i < incoming.m_entity_count; ++i) {
        Entity entity = incoming.m_entities[i];
        if (entity.parent >= 0) {
            entity.parent += static_cast<int32_t>(entity_base);
        }
        if (entity.mesh >= 0) {
            entity.mesh += static_cast<int32_t>(mesh_base);
        }
        if (entity.material >= 0) {
            entity.material += static_cast<int32_t>(material_base);
        }
        entity.dirty = true;
        m_entities[entity_base + i] = entity;
        m_generation[entity_base + i] = 1;
    }

    // Scripts: the type name points into the INCOMING file's buffer, which
    // the caller keeps alive for as long as the world (the same contract as
    // a non-additive load).
    for (uint32_t i = 0; i < incoming.m_script_count; ++i) {
        ScriptRef script = incoming.m_scripts[i];
        script.entity += static_cast<int32_t>(entity_base);
        m_scripts[script_base + i] = script;
    }

    // Rigidbodies rebase on the entity only: mass and damping are values,
    // not indices. The native body they will drive does not exist yet --
    // whoever merges the scene has to create it, exactly as boot does.
    // Animators rebase on the entity AND the controller, since the incoming
    // scene's controllers were appended after the running scene's.
    for (uint32_t i = 0; i < incoming.m_animator_ref_count; ++i) {
        AnimatorRef ar = incoming.m_animator_refs[i];
        ar.entity += static_cast<int32_t>(entity_base);
        ar.controller += controller_base;
        m_animator_refs[animator_ref_base + i] = ar;
    }

    for (uint32_t i = 0; i < incoming.m_rigidbody_count; ++i) {
        RigidbodyRef rb = incoming.m_rigidbodies[i];
        rb.entity += static_cast<int32_t>(entity_base);
        m_rigidbodies[rigidbody_base + i] = rb;
    }

    // Animation: skeletons and clips move across unchanged, but a
    // controller's states name CLIP INDICES, so those rebase too. Getting
    // this wrong would not crash -- the character would simply play some
    // other scene's animation, which is exactly the sort of quiet wrongness
    // worth spelling out.
    for (uint32_t i = 0; i < incoming.m_skeleton_count; ++i) {
        m_skeletons[skeleton_base + i] = incoming.m_skeletons[i];
    }
    for (uint32_t i = 0; i < incoming.m_clip_count; ++i) {
        m_clips[clip_base + i] = incoming.m_clips[i];
    }
    for (uint32_t i = 0; i < incoming.m_controller_count; ++i) {
        anim::Controller controller = incoming.m_controllers[i];
        for (uint32_t s = 0; s < controller.state_count; ++s) {
            controller.states[s].clip =
                static_cast<uint16_t>(controller.states[s].clip + clip_base);
        }
        m_controllers[controller_base + i] = controller;
    }
    for (uint32_t i = 0; i < incoming.m_skinned_mesh_count; ++i) {
        LoadedSkinnedMesh mesh = incoming.m_skinned_meshes[i];
        mesh.material_index += material_base;
        mesh.skeleton += skeleton_base;
        m_skinned_meshes[skinned_mesh_base + i] = mesh;
    }
    for (uint32_t i = 0; i < incoming.m_skinned_count; ++i) {
        SkinnedRenderer renderer = incoming.m_skinned[i];
        renderer.entity += static_cast<int32_t>(entity_base);
        renderer.mesh += static_cast<int32_t>(skinned_mesh_base);
        if (renderer.material >= 0) {
            renderer.material += static_cast<int32_t>(material_base);
        }
        renderer.skeleton += skeleton_base;
        renderer.controller += controller_base;
        // The animator pool is its own index space now, not a parallel array
        // to the renderers, so it rebases on its own base.
        renderer.animator += animator_base;
        m_skinned[skinned_base + i] = renderer;
    }

    m_material_count += incoming.m_material_count;
    m_mesh_count += incoming.m_mesh_count;
    m_entity_count += incoming.m_entity_count;
    m_script_count += incoming.m_script_count;
    m_rigidbody_count += incoming.m_rigidbody_count;
    m_animator_ref_count += incoming.m_animator_ref_count;
    m_skeleton_count += incoming.m_skeleton_count;
    m_clip_count += incoming.m_clip_count;
    m_controller_count += incoming.m_controller_count;
    m_skinned_mesh_count += incoming.m_skinned_mesh_count;
    m_skinned_count += incoming.m_skinned_count;
    m_animator_count += incoming.m_animator_count;

    // Re-bind every animator, not just the new ones: the clip array is a
    // single block and the incoming clips may have moved it, so an animator
    // bound before the merge could be holding a stale base pointer.
    for (uint32_t i = 0; i < m_skinned_count; ++i) {
        const SkinnedRenderer& renderer = m_skinned[i];
        m_animators[renderer.animator].bind(&m_skeletons[renderer.skeleton],
                                            &m_controllers[renderer.controller],
                                            m_clips, m_clip_count);
    }

    // The running scene keeps its own camera and light: the player is
    // looking through them.
    update_world_matrices();
    return true;
}

int32_t World::animator_for_entity(int32_t entity_index) const
{
    // The renderer's own entity first: the direct case, and the only one
    // that existed before M12.5.
    for (uint32_t i = 0; i < m_skinned_count; ++i) {
        if (m_skinned[i].entity == entity_index) {
            return static_cast<int32_t>(m_skinned[i].animator);
        }
    }

    // Then the Animator COMPONENT's entity, which is usually a different one.
    // Unity's model importer puts the Animator on the model root and the
    // SkinnedMeshRenderer on a child mesh object, so a script calling
    // GetComponent<Animator>().Play() addresses the root -- and matching only
    // renderers would miss it and silently do nothing. Walking down from the
    // component to the renderer it drives is what makes the ordinary imported
    // character work.
    for (uint32_t a = 0; a < m_animator_ref_count; ++a) {
        if (m_animator_refs[a].entity != entity_index) {
            continue;
        }
        for (uint32_t i = 0; i < m_skinned_count; ++i) {
            if (is_descendant_of(m_skinned[i].entity, entity_index)) {
                return static_cast<int32_t>(m_skinned[i].animator);
            }
        }
    }
    return -1;
}

bool World::is_descendant_of(int32_t index, int32_t ancestor) const
{
    // Bounded by the entity count: a cycle introduced by runtime reparenting
    // must not hang the frame.
    uint32_t guard = 0;
    while (index >= 0 && guard++ <= kMaxEntities) {
        if (index == ancestor) {
            return true;
        }
        index = m_entities[index].parent;
    }
    return false;
}

int32_t World::state_index(uint32_t controller, uint32_t name_hash) const
{
    if (controller >= m_controller_count) {
        return -1;
    }
    const anim::Controller& c = m_controllers[controller];
    for (uint32_t i = 0; i < c.state_count; ++i) {
        if (c.states[i].name_hash == name_hash) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

void World::update_animators(float dt)
{
    // Advance each ANIMATOR once. Walking the renderers instead would step a
    // shared animator once per renderer -- 19 times a frame for one imported
    // character, which is not a slow clock but a fast one: time, transitions
    // and exit conditions would all run 19x.
    for (uint32_t a = 0; a < m_animator_count; ++a) {
        if (m_animators[a].valid()) {
            m_animators[a].update(dt);
        }
    }

    // Root motion drives the entity, so a walk cycle actually travels (M9
    // task 4). This IS per renderer -- each has its own entity to move -- but
    // only for the first renderer of each animator, because the delta is
    // consumed, not accumulated: applying one animator's delta to 19 entities
    // is right only when they are 19 separate characters, and applying it 19
    // times to the same entity would move it 19x as far.
    for (uint32_t i = 0; i < m_skinned_count; ++i) {
        const anim::Animator& animator = m_animators[m_skinned[i].animator];
        if (!animator.valid()) {
            continue;
        }
        bool first_for_animator = true;
        for (uint32_t j = 0; j < i; ++j) {
            if (m_skinned[j].animator == m_skinned[i].animator &&
                m_skinned[j].entity == m_skinned[i].entity) {
                first_for_animator = false;
                break;
            }
        }
        if (!first_for_animator) {
            continue;
        }
        // The delta is zero unless root motion is enabled, so this is
        // unconditional; it is expressed in the entity's local space, and the
        // rotation delta composes on the entity's rotation.
        const int32_t entity = m_skinned[i].entity;
        if (entity >= 0) {
            const Entity& e = m_entities[entity];
            set_local_position(entity, add(e.pos, animator.root_motion_delta()));
            set_local_rotation(entity, quat_normalize(quat_mul(
                                           animator.root_rotation_delta(), e.rot)));
        }
    }
}

void World::update_world_matrices()
{
    // Dirty tracking (M8 task 1): an entity recomputes only when its own
    // local TRS changed or an ancestor's did. File-ordered scenes resolve in
    // the first pass (parents precede children); runtime reparenting can
    // create forward references, so keep passing until nothing is pending.
    // A cycle (impossible via the public API) leaves 'pending' stuck rather
    // than looping forever.
    bool done[kMaxEntities];
    bool refreshed[kMaxEntities];
    uint32_t pending = 0;
    for (uint32_t i = 0; i < m_entity_count; ++i) {
        done[i] = !m_entities[i].alive;
        refreshed[i] = false;
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
            Entity& e = m_entities[i];
            const int32_t p = e.parent;
            if (p >= 0 && !done[p]) {
                continue;
            }
            const bool need = e.dirty || (p >= 0 && refreshed[p]);
            if (need) {
                const Mat4 local = mat4_trs(e.pos, e.rot, e.scale);
                m_world[i] = p < 0 ? local : mat4_mul(m_world[p], local);
                e.dirty = false;
            }
            refreshed[i] = need;
            done[i] = true;
            ++resolved_this_pass;
        }
        if (resolved_this_pass == 0) {
            break; // cycle or dead parent; leave the rest stale, never hang
        }
        pending -= resolved_this_pass;
    }
}

void World::set_local_position(int32_t index, Vec3 p)
{
    if (index >= 0 && index < static_cast<int32_t>(kMaxEntities)) {
        m_entities[index].pos = p;
        m_entities[index].dirty = true;
    }
}

void World::set_local_rotation(int32_t index, Quat q)
{
    if (index >= 0 && index < static_cast<int32_t>(kMaxEntities)) {
        m_entities[index].rot = q;
        m_entities[index].dirty = true;
    }
}

void World::set_local_scale(int32_t index, Vec3 s)
{
    if (index >= 0 && index < static_cast<int32_t>(kMaxEntities)) {
        m_entities[index].scale = s;
        m_entities[index].dirty = true;
    }
}

void World::set_parent(int32_t index, int32_t parent_index)
{
    if (index >= 0 && index < static_cast<int32_t>(kMaxEntities)) {
        m_entities[index].parent = parent_index;
        m_entities[index].dirty = true;
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
