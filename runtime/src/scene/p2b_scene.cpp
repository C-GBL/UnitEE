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
    for (uint32_t i = 0; i < kMaxAnimators; ++i) {
        m_animator_drives_entities[i] = false;
    }
    m_audio_source_count = 0;
    m_listener_entity = -1;
    m_particle_count = 0;
    m_ui_count = 0;
    m_font_count = 0;
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
            m.clamp = (flags & 8u) != 0u;
            m.sky = (flags & 16u) != 0u;
        }
        m_material_count = count;
    }

    // --- Baked fonts (M12.5) ------------------------------------------------
    //
    // 32-byte header, then 12 bytes per glyph. The pixels live in the TEX
    // section the header names, uploaded by the host like any texture.
    const uint32_t font_sections = file.count_of(io::kSectionFont);
    if (font_sections > kMaxUIFonts) {
        m_error = "too many fonts";
        return false;
    }
    for (uint32_t fi = 0; fi < font_sections; ++fi) {
        const io::P2bSection* fs = file.find(io::kSectionFont, fi);
        const View fv{fs->data, fs->size};
        if (!fv.ok(0, 32u)) {
            m_error = "font header truncated";
            return false;
        }
        gfx::UIFont& font = m_fonts[fi];
        font = gfx::UIFont{};
        font.texture = fv.u32(0);
        font.glyph_count = fv.u32(4);
        font.ascent = fv.f32(8);
        font.line_height = fv.f32(12);
        font.first_char = fv.u32(16);
        if (font.glyph_count > 96u) {
            m_error = "font glyph count out of range";
            return false;
        }
        if (!fv.ok(32u, font.glyph_count * 12u)) {
            m_error = "font glyph table truncated";
            return false;
        }
        for (uint32_t g = 0; g < font.glyph_count; ++g) {
            const uint32_t at = 32u + g * 12u;
            gfx::UIFontGlyph& glyph = font.glyphs[g];
            glyph.u = fv.u16(at + 0);
            glyph.v = fv.u16(at + 2);
            glyph.w = fv.data[at + 4];
            glyph.h = fv.data[at + 5];
            glyph.bearing_x = static_cast<int8_t>(fv.data[at + 6]);
            glyph.bearing_y = static_cast<int8_t>(fv.data[at + 7]);
            glyph.advance_q4 = fv.u16(at + 8);
        }
        m_font_count = fi + 1u;
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

            // Optional 1D blend-tree table (M12.5), appended after the
            // params: u32 count, then per tree u16 state / u8 param /
            // u8 child_count and child_count x (u16 clip, u16 pad,
            // f32 threshold). Absent on older containers -- readers before
            // this stopped at the params, which is what makes the addition
            // compatible in both directions.
            const uint32_t trees_at = params_at + controller.param_count * 4u;
            controller.tree_count = 0;
            if (v.ok(trees_at, 4u)) {
                const uint32_t tree_count = v.u32(trees_at);
                if (tree_count > anim::kMaxBlendTrees) {
                    m_error = "too many blend trees";
                    return false;
                }
                uint32_t at = trees_at + 4u;
                for (uint32_t t = 0; t < tree_count; ++t) {
                    if (!v.ok(at, 4u)) {
                        m_error = "blend tree header truncated";
                        return false;
                    }
                    anim::BlendTreeDef& tree = controller.trees[t];
                    tree.state = v.u16(at + 0);
                    tree.param = sec->data[at + 2];
                    tree.child_count = sec->data[at + 3];
                    at += 4u;
                    if (tree.state >= controller.state_count ||
                        tree.param >= controller.param_count ||
                        tree.child_count < 2u ||
                        tree.child_count > anim::kMaxBlendChildren) {
                        m_error = "blend tree references out of range";
                        return false;
                    }
                    if (!v.ok(at, tree.child_count * 8u)) {
                        m_error = "blend tree children truncated";
                        return false;
                    }
                    for (uint32_t c = 0; c < tree.child_count; ++c) {
                        tree.clip[c] = v.u16(at + 0);
                        tree.threshold[c] = v.f32(at + 4);
                        if (tree.clip[c] >= m_clip_count) {
                            m_error = "blend tree clip out of range";
                            return false;
                        }
                        at += 8u;
                    }
                }
                controller.tree_count = tree_count;
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
        e.follow_camera = (v.u32(at + 52) & kEntityFlagFollowCamera) != 0u;
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
                if (v.ok(data_off, 80u)) {
                    // M14: ambient (r, g, b, pad) after the fog block.
                    m_camera.ambient = Vec3{v.f32(data_off + 64), v.f32(data_off + 68),
                                            v.f32(data_off + 72)};
                }
            } else if (type == kComponentDirectionalLight) {
                // 24-byte v1 payload (a directional light's direction and
                // colour) or the 48-byte M14 payload with kind, range, spot
                // cosine and flags. Every light goes in the table; the
                // first DIRECTIONAL one also fills m_light for older callers.
                if (!v.ok(data_off, 24u)) {
                    m_error = "light payload truncated";
                    return false;
                }
                Light lt;
                lt.entity = static_cast<int32_t>(i);
                lt.colour = Vec3{v.f32(data_off + 12), v.f32(data_off + 16),
                                 v.f32(data_off + 20)};
                if (v.ok(data_off, 48u)) {
                    lt.kind = v.u32(data_off + 24);
                    lt.range = v.f32(data_off + 28);
                    lt.spot_cos = v.f32(data_off + 32);
                    lt.enabled = (v.u32(data_off + 36) & 1u) != 0u;
                }
                if (lt.kind > 2u) {
                    lt.kind = 0u;
                }
                if (lt.kind == 0u && m_light.entity < 0) {
                    m_light.entity = static_cast<int32_t>(i);
                    m_light.dir = Vec3{v.f32(data_off + 0), v.f32(data_off + 4),
                                       v.f32(data_off + 8)};
                    m_light.colour = lt.colour;
                }
                if (m_light_count < kMaxLights) {
                    m_lights[m_light_count++] = lt;
                } else {
                    log(LogLevel::Warn, "scene: more than %u lights; the rest are dropped",
                        static_cast<unsigned>(kMaxLights));
                }
            } else if (type == kComponentShadow) {
                if (!v.ok(data_off, 20u)) {
                    m_error = "shadow payload truncated";
                    return false;
                }
                if (m_shadow_count >= kMaxShadows) {
                    m_error = "too many shadow casters";
                    return false;
                }
                ShadowRef& sh = m_shadows[m_shadow_count];
                sh.entity = static_cast<int32_t>(i);
                sh.mode = v.u32(data_off + 0);
                sh.radius = v.f32(data_off + 4);
                sh.strength = v.f32(data_off + 8);
                sh.max_height = v.f32(data_off + 12);
                e.shadow = static_cast<int16_t>(m_shadow_count);
                ++m_shadow_count;
            } else if (type == kComponentLod) {
                if (!v.ok(data_off, 16u)) {
                    m_error = "lod payload truncated";
                    return false;
                }
                if (m_lod_count >= kMaxLods) {
                    m_error = "too many lod levels";
                    return false;
                }
                LodRef& lr = m_lods[m_lod_count];
                lr.entity = static_cast<int32_t>(i);
                lr.min_height = v.f32(data_off + 0);
                lr.max_height = v.f32(data_off + 4);
                lr.size = v.f32(data_off + 8);
                e.lod = static_cast<int16_t>(m_lod_count);
                ++m_lod_count;
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
            } else if (type == kComponentAudioSource) {
                // 24 bytes: clip, volume, flags, min/max distance, priority.
                if (!v.ok(data_off, 24u)) {
                    m_error = "audio source payload truncated";
                    return false;
                }
                if (m_audio_source_count >= kMaxAudioSources) {
                    m_error = "too many audio sources";
                    return false;
                }
                AudioSourceRef& snd = m_audio_sources[m_audio_source_count];
                snd.entity = static_cast<int32_t>(i);
                snd.clip = v.u32(data_off + 0);
                snd.volume = v.f32(data_off + 4);
                snd.flags = v.u32(data_off + 8);
                snd.min_distance = v.f32(data_off + 12);
                snd.max_distance = v.f32(data_off + 16);
                snd.priority = static_cast<int32_t>(v.u32(data_off + 20));
                ++m_audio_source_count;
            } else if (type == kComponentAudioListener) {
                // 4 bytes, all pad. Unity's rule is one listener; the
                // validator errors on a second at build time, and a scene
                // that ships one anyway keeps the FIRST here rather than
                // failing a load over an authoring slip.
                if (!v.ok(data_off, 4u)) {
                    m_error = "audio listener payload truncated";
                    return false;
                }
                if (m_listener_entity < 0) {
                    m_listener_entity = static_cast<int32_t>(i);
                }
            } else if (type == kComponentParticleSystem) {
                // 64 bytes; the ParticleEmitter fields in declaration order.
                if (!v.ok(data_off, 64u)) {
                    m_error = "particle system payload truncated";
                    return false;
                }
                if (m_particle_count >= kMaxParticleSystems) {
                    m_error = "too many particle systems";
                    return false;
                }
                ParticleEmitter& fx = m_particle_emitters[m_particle_count];
                fx.entity = static_cast<int32_t>(i);
                fx.texture = v.u32(data_off + 0);
                fx.flags = v.u32(data_off + 4);
                fx.emission_rate = v.f32(data_off + 8);
                fx.burst_count = v.u32(data_off + 12);
                fx.shape = v.u32(data_off + 16);
                fx.shape_a = v.f32(data_off + 20);
                fx.shape_b = v.f32(data_off + 24);
                fx.shape_c = v.f32(data_off + 28);
                fx.lifetime = v.f32(data_off + 32);
                fx.speed = v.f32(data_off + 36);
                fx.size_start = v.f32(data_off + 40);
                fx.size_end = v.f32(data_off + 44);
                fx.colour_start = v.u32(data_off + 48);
                fx.colour_end = v.u32(data_off + 52);
                fx.gravity = v.f32(data_off + 56);
                fx.max_particles = v.u32(data_off + 60);
                if (fx.max_particles > kMaxParticlesPerSystem) {
                    fx.max_particles = kMaxParticlesPerSystem;
                }
                ParticleSystemState& fresh =
                    m_particle_states[m_particle_count];
                fresh = ParticleSystemState{};
                fresh.rng ^= static_cast<uint32_t>(i) * 2654435761u;
                fresh.playing = (fx.flags & 2u) != 0u; // playOnAwake
                if (fresh.playing && fx.burst_count > 0) {
                    // Queued, not fired: spawning here would read world
                    // matrices that do not exist yet.
                    fresh.pending_emit = fx.burst_count;
                }
                ++m_particle_count;
            } else if (type == kComponentUIElement) {
                // 84 bytes; UIElement in declaration order, text inline.
                // (36 fixed + 48 text. This check once said 88 while the
                // exporter wrote 84, and the 4-byte overrun only fired when
                // a UI element was the LAST payload in SCEN -- i.e. on the
                // first real Canvas export, not in any synthetic test.)
                if (!v.ok(data_off, 84u)) {
                    m_error = "ui element payload truncated";
                    return false;
                }
                if (m_ui_count >= kMaxUIElements) {
                    m_error = "too many ui elements";
                    return false;
                }
                UIElement& ui = m_ui[m_ui_count];
                ui = UIElement{};
                ui.entity = static_cast<int32_t>(i);
                const uint32_t kind_role = v.u32(data_off + 0);
                ui.kind = static_cast<uint16_t>(kind_role & 0xFFFFu);
                ui.role = static_cast<uint16_t>(kind_role >> 16);
                ui.x = v.f32(data_off + 4);
                ui.y = v.f32(data_off + 8);
                ui.w = v.f32(data_off + 12);
                ui.h = v.f32(data_off + 16);
                ui.colour = v.u32(data_off + 20);
                ui.texture = v.u32(data_off + 24);
                ui.link = v.i32(data_off + 28);
                // Low byte: font scale. Bits 8-9 / 10-11: horizontal and
                // vertical Text.alignment. Bits 16-23: FONT table index
                // plus one, zero meaning the builtin 8x8 font (old files
                // carry zeros everywhere = the old behaviour).
                const uint32_t scale_bits = v.u32(data_off + 32);
                ui.text_scale = scale_bits & 0xFFu;
                if (ui.text_scale < 1u) {
                    ui.text_scale = 1u;
                }
                ui.align_h = static_cast<uint8_t>((scale_bits >> 8) & 3u);
                ui.align_v = static_cast<uint8_t>((scale_bits >> 10) & 3u);
                const uint32_t font_ref = (scale_bits >> 16) & 0xFFu;
                ui.font = static_cast<int16_t>(font_ref) - 1;
                if (ui.font >= static_cast<int16_t>(m_font_count)) {
                    m_error = "ui element names a missing font";
                    return false;
                }
                for (uint32_t b = 0; b < kMaxUITextLength; ++b) {
                    ui.text[b] =
                        static_cast<char>(v.data[data_off + 36u + b]);
                }
                ui.text[kMaxUITextLength - 1] = '\0';
                ++m_ui_count;
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

    // Resolve each Animator COMPONENT to a pool slot (M12.5). A rig with
    // skinned renderers shares theirs. A rig with none is a rigid-bound
    // model -- meshes parented to bones, no skin weights anywhere, which is
    // how Unity's transform animation ships characters too -- and gets a
    // slot that drives ENTITY transforms: each skeleton bone is matched by
    // name hash to an entity under the Animator, and the sampled pose is
    // written to those entities every frame. Humanoid-with-valid-avatar
    // says nothing about skinning; this is the case it describes.
    for (uint32_t a = 0; a < m_animator_ref_count; ++a) {
        AnimatorRef& ar = m_animator_refs[a];
        int32_t shared = -1;
        for (uint32_t i = 0; i < m_skinned_count && shared < 0; ++i) {
            if (m_skinned[i].entity == ar.entity ||
                is_descendant_of(m_skinned[i].entity, ar.entity)) {
                shared = static_cast<int32_t>(m_skinned[i].animator);
            }
        }
        if (shared >= 0) {
            ar.animator = shared;
            continue;
        }
        if (m_skeleton_count == 0 || ar.controller >= m_controller_count) {
            // An Animator with no rig at all; the exporter warns about this
            // at build time, and GetComponent<Animator>() returns null.
            continue;
        }
        if (m_animator_count >= kMaxAnimators) {
            m_error = "too many distinct animators";
            return false;
        }
        const uint32_t slot = m_animator_count++;
        // The exporter bakes one rig per scene, so a transform-animated rig
        // is always skeleton 0 -- the same convention SKMS headers use.
        m_animators[slot].bind(&m_skeletons[0], &m_controllers[ar.controller],
                               m_clips, m_clip_count);
        ar.animator = static_cast<int32_t>(slot);
        m_animator_drives_entities[slot] = true;
        const anim::Skeleton& skeleton = m_skeletons[0];
        for (uint32_t b = 0; b < skeleton.bone_count; ++b) {
            m_bone_entity[slot][b] = -1;
            for (uint32_t e = 0; e < m_entity_count; ++e) {
                if (!m_entities[e].alive ||
                    m_entities[e].name_hash != skeleton.bones[b].name_hash) {
                    continue;
                }
                if (!is_descendant_of(static_cast<int32_t>(e), ar.entity)) {
                    continue;
                }
                m_bone_entity[slot][b] = static_cast<int16_t>(e);
                break;
            }
        }
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
    } else if (m_audio_source_count + incoming.m_audio_source_count >
               kMaxAudioSources) {
        m_error = "additive scene does not fit: audio sources";
    } else if (m_particle_count + incoming.m_particle_count >
               kMaxParticleSystems) {
        m_error = "additive scene does not fit: particle systems";
    } else if (m_font_count + incoming.m_font_count > kMaxUIFonts) {
        m_error = "additive scene does not fit: fonts";
    } else if (m_ui_count + incoming.m_ui_count > kMaxUIElements) {
        m_error = "additive scene does not fit: ui elements";
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
    const uint32_t light_base = m_light_count;
    const uint32_t shadow_base = m_shadow_count;
    const uint32_t lod_base = m_lod_count;
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
        if (entity.shadow >= 0) {
            entity.shadow = static_cast<int16_t>(entity.shadow + static_cast<int32_t>(shadow_base));
        }
        if (entity.lod >= 0) {
            entity.lod = static_cast<int16_t>(entity.lod + static_cast<int32_t>(lod_base));
        }
        entity.dirty = true;
        m_entities[entity_base + i] = entity;
        m_generation[entity_base + i] = 1;
    }
    // M14 tables ride along with their entities; over capacity, the extras
    // are dropped with a log line rather than failing the whole load.
    for (uint32_t i = 0; i < incoming.m_light_count; ++i) {
        if (m_light_count >= kMaxLights) {
            log(LogLevel::Warn, "scene: additive load drops a light (table full)");
            break;
        }
        Light lt = incoming.m_lights[i];
        lt.entity += static_cast<int32_t>(entity_base);
        m_lights[m_light_count++] = lt;
    }
    for (uint32_t i = 0; i < incoming.m_shadow_count; ++i) {
        if (m_shadow_count >= kMaxShadows) {
            log(LogLevel::Warn, "scene: additive load drops a shadow (table full)");
            break;
        }
        ShadowRef sh = incoming.m_shadows[i];
        sh.entity += static_cast<int32_t>(entity_base);
        m_shadows[m_shadow_count++] = sh;
    }
    for (uint32_t i = 0; i < incoming.m_lod_count; ++i) {
        if (m_lod_count >= kMaxLods) {
            log(LogLevel::Warn, "scene: additive load drops a lod level (table full)");
            break;
        }
        LodRef lr = incoming.m_lods[i];
        lr.entity += static_cast<int32_t>(entity_base);
        m_lods[m_lod_count++] = lr;
    }
    (void)light_base;

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
        if (ar.animator >= 0) {
            ar.animator += static_cast<int32_t>(animator_base);
        }
        m_animator_refs[animator_ref_base + i] = ar;
    }
    // Transform-animation state rides with its slot: the drive flag and the
    // bone->entity map, the latter rebased onto the merged entity table.
    for (uint32_t s = 0; s < incoming.m_animator_count; ++s) {
        const uint32_t dst = animator_base + s;
        m_animator_drives_entities[dst] = incoming.m_animator_drives_entities[s];
        for (uint32_t b = 0; b < anim::kMaxBones; ++b) {
            const int16_t e = incoming.m_bone_entity[s][b];
            m_bone_entity[dst][b] =
                e >= 0 ? static_cast<int16_t>(e + entity_base) : int16_t(-1);
        }
    }

    // Audio sources rebase their entity only. Their clip indices refer to
    // the BOOT scene's SND section -- an additive scene's own sounds are not
    // merged (nothing reloads SND clips mid-run yet), which the exporter
    // warns about rather than letting indices silently alias.
    for (uint32_t i = 0; i < incoming.m_audio_source_count; ++i) {
        AudioSourceRef snd = incoming.m_audio_sources[i];
        snd.entity += static_cast<int32_t>(entity_base);
        m_audio_sources[m_audio_source_count++] = snd;
    }

    for (uint32_t i = 0; i < incoming.m_particle_count; ++i) {
        ParticleEmitter fx = incoming.m_particle_emitters[i];
        fx.entity += static_cast<int32_t>(entity_base);
        m_particle_emitters[m_particle_count] = fx;
        m_particle_states[m_particle_count] = incoming.m_particle_states[i];
        ++m_particle_count;
    }

    const uint32_t ui_base = m_ui_count;
    const uint32_t font_base = m_font_count;
    for (uint32_t i = 0; i < incoming.m_font_count; ++i) {
        // Copied verbatim like materials: the texture index is relative to
        // the incoming FILE's TEX sections, the same caveat additive
        // material textures carry.
        m_fonts[m_font_count++] = incoming.m_fonts[i];
    }
    for (uint32_t i = 0; i < incoming.m_ui_count; ++i) {
        UIElement ui = incoming.m_ui[i];
        ui.entity += static_cast<int32_t>(entity_base);
        if (ui.link >= 0) {
            ui.link += static_cast<int32_t>(ui_base);
        }
        if (ui.font >= 0) {
            ui.font = static_cast<int16_t>(ui.font + font_base);
        }
        m_ui[m_ui_count++] = ui;
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
    // Transform-driving slots re-bind through the component that owns them.
    // An incoming rig's skeleton 0 landed at skeleton_base; one already ours
    // keeps skeleton 0. Both cases are "the ref's controller, the rig's own
    // skeleton", which the slot index against animator_base distinguishes.
    for (uint32_t a = 0; a < m_animator_ref_count; ++a) {
        const AnimatorRef& ar = m_animator_refs[a];
        if (ar.animator < 0 ||
            !m_animator_drives_entities[static_cast<uint32_t>(ar.animator)]) {
            continue;
        }
        const uint32_t skeleton =
            static_cast<uint32_t>(ar.animator) >= animator_base ? skeleton_base
                                                                : 0u;
        m_animators[ar.animator].bind(&m_skeletons[skeleton],
                                      &m_controllers[ar.controller], m_clips,
                                      m_clip_count);
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

    // Then the Animator COMPONENT's entity, which is usually a different one:
    // Unity's model importer puts the Animator on the model root and the
    // renderers on children, so a script calling GetComponent<Animator>()
    // addresses the root. Load resolved every component to its slot --
    // shared with the rig's skinned renderers, or a transform-driving slot
    // of its own for a rigid-bound model (M12.5).
    for (uint32_t a = 0; a < m_animator_ref_count; ++a) {
        if (m_animator_refs[a].entity == entity_index &&
            m_animator_refs[a].animator >= 0) {
            return m_animator_refs[a].animator;
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
        if (!m_animators[a].valid()) {
            continue;
        }
        m_animators[a].update(dt);
        if (!m_animator_drives_entities[a]) {
            continue;
        }
        // Transform animation (M12.5): the pose IS the entity transforms.
        // set_local_* marks each entity dirty, so the matrix pass after this
        // recomputes exactly the subtree that moved.
        const anim::Skeleton* skeleton = m_animators[a].skeleton();
        const anim::Pose& pose = m_animators[a].pose();
        for (uint32_t b = 0; b < skeleton->bone_count; ++b) {
            const int16_t entity = m_bone_entity[a][b];
            if (entity < 0) {
                continue;
            }
            set_local_position(entity, pose.pos[b]);
            set_local_rotation(entity, pose.rot[b]);
            set_local_scale(entity, pose.scale[b]);
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


int32_t World::ui_element_for_entity(int32_t entity_index) const
{
    for (uint32_t i = 0; i < m_ui_count; ++i) {
        if (m_ui[i].entity == entity_index) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

void World::ui_set_rect(uint32_t i, float x, float y, float w, float h)
{
    if (i < m_ui_count) {
        m_ui[i].x = x;
        m_ui[i].y = y;
        m_ui[i].w = w;
        m_ui[i].h = h;
    }
}

void World::ui_set_colour(uint32_t i, uint32_t rgba)
{
    if (i < m_ui_count) {
        m_ui[i].colour = rgba;
    }
}

int32_t World::light_for_entity(int32_t entity_index) const
{
    for (uint32_t i = 0; i < m_light_count; ++i) {
        if (m_lights[i].entity == entity_index) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

void World::set_light(int32_t entity_index, uint32_t kind, Vec3 colour, float range,
                      float spot_cos, bool enabled)
{
    if (entity_index < 0 || static_cast<uint32_t>(entity_index) >= m_entity_count) {
        return;
    }
    int32_t at = light_for_entity(entity_index);
    if (at < 0) {
        if (m_light_count >= kMaxLights) {
            log(LogLevel::Warn, "scene: light table full (%u); Light on entity %d ignored",
                static_cast<unsigned>(kMaxLights), static_cast<int>(entity_index));
            return;
        }
        at = static_cast<int32_t>(m_light_count++);
        m_lights[at] = Light{};
        m_lights[at].entity = entity_index;
    }
    Light& lt = m_lights[at];
    lt.kind = kind > 2u ? 0u : kind;
    lt.colour = colour;
    lt.range = range;
    lt.spot_cos = spot_cos;
    lt.enabled = enabled;
    if (lt.kind == 0u && (m_light.entity < 0 || m_light.entity == entity_index)) {
        m_light.entity = entity_index;
        m_light.colour = colour;
    }
}

void World::ui_set_text_glow(uint32_t i, float spread, float intensity, float dilate)
{
    if (i < m_ui_count) {
        UIElement& e = m_ui[i];
        e.glow_spread = spread < 0.0f ? 0.0f : spread;
        e.glow_intensity = intensity < 0.0f ? 0.0f : (intensity > 1.0f ? 1.0f : intensity);
        e.glow_dilate = dilate < 0.0f ? 0.0f : (dilate > 1.0f ? 1.0f : dilate);
    }
}

void World::ui_set_text(uint32_t i, const char* text)
{
    if (i >= m_ui_count || text == nullptr) {
        return;
    }
    uint32_t b = 0;
    for (; b + 1 < kMaxUITextLength && text[b] != '\0'; ++b) {
        m_ui[i].text[b] = text[b];
    }
    m_ui[i].text[b] = '\0';
}

void World::ui_set_visible(uint32_t i, bool visible)
{
    if (i < m_ui_count) {
        m_ui[i].visible = visible;
    }
}

} // namespace scene
} // namespace ps2ur
