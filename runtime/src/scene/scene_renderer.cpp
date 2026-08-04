#include "ps2ur/scene_renderer.h"

#include "ps2ur/gs_batch.h"
#include "ps2ur/log.h"

#include <cstring>

namespace ps2ur {
namespace scene {

namespace {

// 0..16 lit block, 17 fog, 18..113 the M9 bone palette (24 x 4 qwords).
alignas(16) gfx::Qword g_constants[18 + anim::kMaxPaletteBones * 4];
alignas(16) Mat4 g_palette[anim::kMaxPaletteBones];

void set_float4(gfx::Qword& q, float x, float y, float z, float w)
{
    union {
        float f;
        uint32_t u;
    } cx{x}, cy{y}, cz{z}, cw{w};
    q.lo = static_cast<uint64_t>(cx.u) | (static_cast<uint64_t>(cy.u) << 32);
    q.hi = static_cast<uint64_t>(cz.u) | (static_cast<uint64_t>(cw.u) << 32);
}

// Conservative world-space radius: local radius scaled by the longest world
// basis column (handles non-uniform scale by over-approximating).
float world_radius(const Mat4& w, float local_radius)
{
    const float cx = w.m[0] * w.m[0] + w.m[1] * w.m[1] + w.m[2] * w.m[2];
    const float cy = w.m[4] * w.m[4] + w.m[5] * w.m[5] + w.m[6] * w.m[6];
    const float cz = w.m[8] * w.m[8] + w.m[9] * w.m[9] + w.m[10] * w.m[10];
    float longest_sq = cx > cy ? cx : cy;
    if (cz > longest_sq) {
        longest_sq = cz;
    }
    return local_radius * __builtin_sqrtf(longest_sq);
}

uint32_t program_for_kind(const RendererPrograms& programs, uint32_t kind)
{
    switch (kind) {
        case kMaterialUnlitTextured:
            return programs.tex_addr;
        case kMaterialVertexLit:
        case kMaterialLitAlpha:
        case kMaterialCutout:
            return programs.lit_addr;
        case kMaterialVertexLitFog:
            return programs.lit_fog_addr;
        default:
            return programs.unlit_addr; // Unlit, Additive
    }
}

bool kind_uses_lit_constants(uint32_t kind)
{
    return kind == kMaterialVertexLit || kind == kMaterialLitAlpha ||
           kind == kMaterialCutout || kind == kMaterialVertexLitFog;
}


// Packs four floats into a qword, for the CPU-staged particle vertices.
inline gfx::Qword qword4f(float x, float y, float z, float w)
{
    union {
        float f;
        uint32_t u;
    } cx{x}, cy{y}, cz{z}, cw{w};
    gfx::Qword q;
    q.lo = static_cast<uint64_t>(cx.u) | (static_cast<uint64_t>(cy.u) << 32);
    q.hi = static_cast<uint64_t>(cz.u) | (static_cast<uint64_t>(cw.u) << 32);
    return q;
}

} // namespace

bool SceneRenderer::init_ui(gfx::GsDevice& device)
{
    // Call BETWEEN frames. The font atlas upload is GS packet data, and
    // packet writes only reach VRAM inside a kicked frame -- framing the
    // upload here removes the trap. The first real Canvas hit it: init_ui
    // ran with no frame open, the upload died in a stale buffer, and every
    // glyph sampled whatever the atlas address happened to hold.
    device.begin_frame();
    m_ui_ready = m_ui_overlay.init(device);
    device.end_frame(/*flip=*/false);
    return m_ui_ready;
}

bool SceneRenderer::render(gfx::GsDevice& device, gfx::DmaChain& chain,
                           World& world, const RendererPrograms& programs,
                           BindTextureFn bind_texture, void* bind_user,
                           RenderStats* stats, OverlayFn overlay,
                           void* overlay_user)
{
    RenderStats local{};
    if (!world.has_camera()) {
        return false;
    }
    const Camera& cam = world.camera();
    const uint32_t cam_entity = static_cast<uint32_t>(cam.entity);

    // --- Camera matrices (M8 task 2) ---------------------------------------
    const float screen_w = static_cast<float>(device.config().width);
    const float screen_h = static_cast<float>(device.config().height);
    const float vp_x = cam.viewport[0] * screen_w;
    const float vp_y = cam.viewport[1] * screen_h;
    const float vp_w = cam.viewport[2] * screen_w;
    const float vp_h = cam.viewport[3] * screen_h;
    const float aspect = vp_w / vp_h;

    const Mat4 proj =
        cam.orthographic
            ? mat4_ortho(cam.ortho_size * aspect, cam.ortho_size, cam.znear, cam.zfar)
            : mat4_perspective(cam.fov, aspect, cam.znear, cam.zfar);
    // Unity scenes are left-handed looking down +z; the projection is
    // right-handed. flipz converts; winding mirrors, harmless without
    // backface culling on the GS.
    Mat4 flipz = mat4_identity();
    flipz.m[10] = -1.0f;
    const Mat4 view = mat4_rigid_inverse(world.world_matrix(cam_entity));
    const Mat4 flipped_view = mat4_mul(flipz, view);
    const Mat4 viewproj = mat4_mul(proj, flipped_view);
    const FrustumPlanes frustum = frustum_from_viewproj(viewproj);

    // Screen mapping into the viewport rectangle (12.4 fixed point, origin
    // at the GS 2048 centre).
    const float sx = vp_w * 0.5f;
    const float sy = -vp_h * 0.5f;
    const float zmax = 8388607.0f;
    const float szf = -zmax * 0.5f / 16.0f;
    const float ozf = zmax * 0.5f / 16.0f;
    float vscale[3] = {sx, sy, szf};
    float voffset[3] = {vp_x + sx + 2048.0f, vp_y - sy + 2048.0f, ozf};

    // --- Clear + per-frame GS state -----------------------------------------
    device.begin_frame();
    if (cam.clear_flags != 2u) {
        device.clear(cam.clear_r, cam.clear_g, cam.clear_b);
    } else {
        device.clear(0, 0, 0); // depth-only clear still resets Z; colour black
    }
    if (cam.fog_enabled) {
        device.set_fog_colour(cam.fog_r, cam.fog_g, cam.fog_b);
    }
    if (overlay != nullptr) {
        overlay(overlay_user);
    }
    // NO flip here. The geometry below goes through the caller's DmaChain,
    // so this buffer is not complete until the last kick has been waited on;
    // flipping now would put a cleared buffer on screen and draw into it
    // live -- per-object flicker. This is the rule gs_device.h states, the
    // one main_game.cpp learned in M12, and the one this renderer broke for
    // every sample since M8: the goldens never caught it because readback
    // happens after the frame completes, and DISPLAY timing is invisible to
    // a CRC (a golden pins determinism, not correctness -- fifth instance).
    device.end_frame(/*flip=*/false);

    // --- Cull + queue (M8 tasks 3/4) ----------------------------------------
    m_queue.clear();
    const float inv_depth_range = 1.0f / (cam.zfar - cam.znear);
    for (uint32_t e = 0; e < world.entity_count(); ++e) {
        const Entity& ent = world.entity(e);
        if (!ent.alive || ent.mesh < 0) {
            continue;
        }
        if (!world.entity_visible(static_cast<int32_t>(e))) {
            continue;
        }
        ++local.considered;
        if (((1u << (ent.layer & 31u)) & cam.layer_mask) == 0u) {
            ++local.culled;
            continue;
        }
        const LoadedMesh& mesh = world.mesh(static_cast<uint32_t>(ent.mesh));
        const uint32_t mat_index =
            ent.material >= 0 ? static_cast<uint32_t>(ent.material)
                              : mesh.material_index;
        const LoadedMaterial& mat = world.material(mat_index);

        const Mat4& w = world.world_matrix(e);
        const Vec4 c = mat4_mul_vec4(
            w, Vec4{mesh.bounds_center.x, mesh.bounds_center.y,
                    mesh.bounds_center.z, 1.0f});
        const float radius = world_radius(w, mesh.bounds_radius);
        if (frustum_culls_sphere(frustum, Vec3{c.x, c.y, c.z}, radius)) {
            ++local.culled;
            continue;
        }

        const Vec4 vz = mat4_mul_vec4(flipped_view, c);
        const float depth01 = (vz.z - cam.znear) * inv_depth_range;
        const uint32_t pass = mat.transparent ? 1u : 0u;
        const uint32_t tex1 =
            mat.texture_index == 0xFFFFFFFFu ? 0u : mat.texture_index + 1u;
        if (!m_queue.push(pass, mat.kind, tex1, depth01,
                          static_cast<uint16_t>(e),
                          static_cast<uint16_t>(ent.mesh),
                          static_cast<uint16_t>(mat_index))) {
            log(LogLevel::Error, "scene_renderer: queue overflow");
            return false;
        }
    }
    m_queue.sort();

    // --- Emit (grouped) -----------------------------------------------------
    uint32_t current_group = 0xFFFFFFFFu; // (pass<<16 | kind<<8 | tex)
    bool chain_open = false;

    for (uint32_t i = 0; i < m_queue.count(); ++i) {
        const gfx::DrawCommand& cmd = m_queue.command(i);
        const LoadedMesh& mesh = world.mesh(cmd.mesh);
        const LoadedMaterial& mat = world.material(cmd.material);
        const uint32_t tex1 =
            mat.texture_index == 0xFFFFFFFFu ? 0u : mat.texture_index + 1u;
        // The TEST bit keeps a textured-cutout material (tex layout + alpha
        // test, M12.5) from sharing a state group with a plain textured one
        // over the same texture: same kind, same tex, different TEST_1.
        const uint32_t group = ((mat.gs_test != 0 ? 1u : 0u) << 17) |
                               ((mat.transparent ? 1u : 0u) << 16) |
                               (mat.kind << 8) | tex1;

        if (group != current_group) {
            if (chain_open) {
                if (!chain.kick()) {
                    return false;
                }
                chain.wait();
                ++local.kicks;
                chain_open = false;
            }
            // Material + texture state ride PATH3 between kicks: never
            // racing PATH1 (plan 3.4).
            device.packet().reset();
            device.set_material_state(mat.gs_test, mat.gs_alpha, mat.blend,
                                      mat.zwrite);
            if (tex1 != 0u && bind_texture != nullptr) {
                uint32_t tw = 0, th = 0;
                bind_texture(bind_user, mat.texture_index, &tw, &th);
            }
            device.flush_packet();
            current_group = group;
        }
        if (!chain_open) {
            chain.begin();
            chain_open = true;
        }

        const Mat4 mvp = mat4_mul(viewproj, world.world_matrix(cmd.entity));
        bool ok;
        if (kind_uses_lit_constants(mat.kind) && world.has_light()) {
            const Mat4& w = world.world_matrix(cmd.entity);
            const Vec3 ld = world.light().dir;
            const Vec3 obj = normalize(Vec3{
                -(w.m[0] * ld.x + w.m[1] * ld.y + w.m[2] * ld.z),
                -(w.m[4] * ld.x + w.m[5] * ld.y + w.m[6] * ld.z),
                -(w.m[8] * ld.x + w.m[9] * ld.y + w.m[10] * ld.z)});
            const Vec3 lc = world.light().colour;
            gfx::BatchBuilder::build_unlit_constants(mvp.m, vscale, voffset,
                                                     4095.0f, cam.znear,
                                                     g_constants);
            set_float4(g_constants[7], 0, 0, 0, 0);
            set_float4(g_constants[8], 0, 0, 0, 0);
            set_float4(g_constants[9], obj.x, 0, 0, 0);
            set_float4(g_constants[10], obj.y, 0, 0, 0);
            set_float4(g_constants[11], obj.z, 0, 0, 0);
            // Qwords 12..14 are the COLOURS OF LIGHTS 0..2 as (r,g,b,0) --
            // the microprogram broadcasts N.L per light and spends its
            // fourth MADD on ambient. Packing them per-channel instead lit
            // only the red channel (verify-log M9).
            //
            // Scale discipline: exported vertex colours are 0..255, so the
            // light factor stays ~0..1 (verify-log M8).
            set_float4(g_constants[12], lc.x, lc.y, lc.z, 0);
            set_float4(g_constants[13], 0, 0, 0, 0);
            set_float4(g_constants[14], 0, 0, 0, 0);
            set_float4(g_constants[15], 0.157f, 0.157f, 0.157f, 0);
            set_float4(g_constants[16], 255.0f, 255.0f, 255.0f, 128.0f);
            if (mat.kind == kMaterialVertexLitFog) {
                // f = clamp(w*scale + offset, 0, 255); disabled fog means
                // scale 0 / offset 255: F=255 everywhere, i.e. no fog.
                float fog_scale = 0.0f;
                float fog_offset = 255.0f;
                if (cam.fog_enabled && cam.fog_far > cam.fog_near) {
                    const float inv = 1.0f / (cam.fog_far - cam.fog_near);
                    fog_scale = -255.0f * inv;
                    fog_offset = 255.0f * cam.fog_far * inv;
                }
                set_float4(g_constants[17], fog_scale, fog_offset, 255.0f, 0.0f);
                ok = chain.add_constants(g_constants, 18, 0);
            } else {
                ok = chain.add_constants(g_constants, 17, 0);
            }
        } else {
            gfx::BatchBuilder::build_unlit_constants(mvp.m, vscale, voffset,
                                                     4095.0f, cam.znear,
                                                     g_constants);
            ok = chain.add_constants(g_constants, 7, 0);
        }
        const uint32_t addr = program_for_kind(programs, mat.kind);
        for (uint32_t b = 0; b < mesh.batch_count && ok; ++b) {
            ok = chain.add_batch(mesh.batches[b], addr);
        }
        if (!ok) {
            return false;
        }
        ++local.drawn;
    }
    if (chain_open) {
        if (!chain.kick()) {
            return false;
        }
        chain.wait();
        ++local.kicks;
        chain_open = false;
    }

    // --- Skinned pass (M9) --------------------------------------------------
    //
    // One constants upload per character carries the MVP block AND the 24
    // matrix palette (qwords 18..113), then every batch of that character
    // unpacks its vertices above it. The palette is per-batch in principle,
    // but a character whose whole bone set fits one table -- the common case
    // and the M9 acceptance case -- uploads it once and draws every batch
    // against it.
    if (world.skinned_renderer_count() > 0) {
        device.packet().reset();
        device.set_material_state(0, 0, false, true);
        device.flush_packet();
    }
    for (uint32_t s = 0; s < world.skinned_renderer_count(); ++s) {
        const SkinnedRenderer& renderer = world.skinned_renderer(s);
        if (renderer.entity < 0 || renderer.mesh < 0) {
            continue;
        }
        const uint32_t entity = static_cast<uint32_t>(renderer.entity);
        if (!world.entity(entity).alive ||
            !world.entity_visible(renderer.entity)) {
            continue;
        }
        const LoadedSkinnedMesh& mesh =
            world.skinned_mesh(static_cast<uint32_t>(renderer.mesh));
        const anim::Animator& animator = world.animator(renderer.animator);

        // Cull the whole character on its bounding sphere, grown to cover
        // the animation: a posed limb reaches past the bind-pose bounds.
        const Mat4& w = world.world_matrix(entity);
        const Vec4 centre = mat4_mul_vec4(
            w, Vec4{mesh.bounds_center.x, mesh.bounds_center.y,
                    mesh.bounds_center.z, 1.0f});
        const float radius = world_radius(w, mesh.bounds_radius) * 1.5f;
        ++local.considered;
        if (frustum_culls_sphere(frustum, Vec3{centre.x, centre.y, centre.z},
                                 radius)) {
            ++local.culled;
            continue;
        }

        // Textured characters (M12.5): the mesh's format decides the PROGRAM
        // -- a 6-qword blob through the 5-qword program is garbage, whatever
        // the material says -- and the material supplies the texture to bind.
        // Binding happens here, before this renderer's chain traffic starts,
        // the same between-kicks rule the queue's groups follow. The bind
        // MUST be flushed before the chain kicks: set_texture_indexed only
        // appends to the direct packet, and an unflushed TEX0 leaves the
        // character drawing with whatever the previous pass bound last --
        // in a scene with UI, the font atlas.
        const int32_t skin_mat = renderer.material >= 0
                                     ? renderer.material
                                     : static_cast<int32_t>(mesh.material_index);
        if (mesh.textured && bind_texture != nullptr && skin_mat >= 0 &&
            static_cast<uint32_t>(skin_mat) < world.material_count()) {
            const LoadedMaterial& sm =
                world.material(static_cast<uint32_t>(skin_mat));
            if (sm.texture_index != 0xFFFFFFFFu) {
                uint32_t tw = 0, th = 0;
                device.packet().reset();
                if (bind_texture(bind_user, sm.texture_index, &tw, &th)) {
                    device.flush_packet();
                }
            }
        }
        const uint32_t skin_program =
            mesh.textured ? programs.skin_tex_addr : programs.skin_addr;

        const Mat4 mvp = mat4_mul(viewproj, w);
        gfx::BatchBuilder::build_unlit_constants(mvp.m, vscale, voffset, 4095.0f,
                                                 cam.znear, g_constants);
        if (world.has_light()) {
            const Vec3 ld = world.light().dir;
            const Vec3 obj = normalize(Vec3{
                -(w.m[0] * ld.x + w.m[1] * ld.y + w.m[2] * ld.z),
                -(w.m[4] * ld.x + w.m[5] * ld.y + w.m[6] * ld.z),
                -(w.m[8] * ld.x + w.m[9] * ld.y + w.m[10] * ld.z)});
            const Vec3 lc = world.light().colour;
            set_float4(g_constants[9], obj.x, 0, 0, 0);
            set_float4(g_constants[10], obj.y, 0, 0, 0);
            set_float4(g_constants[11], obj.z, 0, 0, 0);
            set_float4(g_constants[12], lc.x, lc.y, lc.z, 0);
        } else {
            set_float4(g_constants[9], 0, 0, 0, 0);
            set_float4(g_constants[10], 0, 0, 0, 0);
            set_float4(g_constants[11], 0, 0, 0, 0);
            set_float4(g_constants[12], 0, 0, 0, 0);
        }
        set_float4(g_constants[13], 0, 0, 0, 0);
        set_float4(g_constants[14], 0, 0, 0, 0);
        set_float4(g_constants[15], 0.157f, 0.157f, 0.157f, 0);
        set_float4(g_constants[16], 255.0f, 255.0f, 255.0f, 128.0f);
        set_float4(g_constants[17], 0, 0, 0, 0);

        uint32_t last_table = 0xFFFFFFFFu;
        chain.begin();
        chain_open = true;
        bool ok = true;
        for (uint32_t b = 0; b < mesh.batch_count && ok; ++b) {
            const uint32_t table_count = mesh.bone_count[b];
            // Re-upload the palette only when this batch's bone table
            // differs from the one already resident.
            if (last_table == 0xFFFFFFFFu ||
                mesh.bone_table[b][0] != mesh.bone_table[last_table][0] ||
                table_count != mesh.bone_count[last_table]) {
                anim::build_palette(animator, mesh.bone_table[b], table_count,
                                    g_palette);
                for (uint32_t slot = 0; slot < table_count; ++slot) {
                    for (uint32_t c = 0; c < 4; ++c) {
                        set_float4(g_constants[18u + slot * 4u + c],
                                   g_palette[slot].m[c * 4 + 0],
                                   g_palette[slot].m[c * 4 + 1],
                                   g_palette[slot].m[c * 4 + 2],
                                   g_palette[slot].m[c * 4 + 3]);
                    }
                }
                ok = chain.add_constants(g_constants, 18u + table_count * 4u, 0);
                last_table = b;
            }
            if (ok) {
                ok = chain.add_batch(mesh.batches[b], skin_program);
                ++local.skin_batches;
            }
        }
        if (!ok || !chain.kick()) {
            return false;
        }
        chain.wait();
        chain_open = false;
        ++local.kicks;
        ++local.skinned_drawn;
    }

    // --- Particles (M12.5 task 4, ADR-011) ----------------------------------
    //
    // CPU-billboarded quads through the vu_unlit_tex path: for each system,
    // stage [tag][count][pos,st,colour x verts] into a static scratch buffer,
    // then constants + batches + kick, waiting before the scratch is reused.
    // Camera right/up come from the view matrix's rows -- the transpose of
    // the camera's rotation -- so the quads face the camera by construction.
    if (world.particle_system_count() > 0) {
        // 13 quads x 6 verts x 3 qwords = 234, under the 255-qword VIF NUM
        // ceiling; one system stages at most 10 batches of that.
        constexpr uint32_t kQuadsPerBatch = 13;
        constexpr uint32_t kBatchQwords = 2 + kQuadsPerBatch * 6 * 3;
        constexpr uint32_t kMaxBatches =
            (kMaxParticlesPerSystem + kQuadsPerBatch - 1) / kQuadsPerBatch;
        alignas(16) static gfx::Qword scratch[kMaxBatches * kBatchQwords];

        const Vec3 cam_right{view.m[0], view.m[4], view.m[8]};
        const Vec3 cam_up{view.m[1], view.m[5], view.m[9]};

        for (uint32_t s = 0; s < world.particle_system_count(); ++s) {
            const ParticleEmitter& emitter = world.particle_emitter(s);
            const ParticleSystemState& state = world.particle_state(s);
            if (state.count == 0 || emitter.entity < 0 ||
                !world.entity_visible(emitter.entity)) {
                continue;
            }
            const bool world_space = (emitter.flags & 8u) != 0u;
            const Mat4& model =
                world.world_matrix(static_cast<uint32_t>(emitter.entity));
            // Bind + state ride the direct packet and must flush BEFORE this
            // system's chain kicks (the same rule as the skinned pass above;
            // unflushed, they land a pass late).
            device.packet().reset();
            bool textured =
                emitter.texture != 0xFFFFFFFFu && bind_texture != nullptr;
            if (textured) {
                uint32_t tw = 0, th = 0;
                textured = bind_texture(bind_user, emitter.texture, &tw, &th);
            }
            // Transparent-pass state: Z test on through the default TEST,
            // no Z write, blend on. 0x44 = (Cs-Cd)*As+Cd, 0x48 = Cs*As+Cd.
            device.set_material_state(
                0, (emitter.flags & 4u) != 0u ? 0x48u : 0x44u, true, false);
            device.flush_packet();

            uint32_t emitted = 0;
            uint32_t batch_count = 0;
            gfx::Qword* cursor = scratch;
            gfx::BatchBlock batches[kMaxBatches];
            while (emitted < state.count) {
                const uint32_t quads =
                    state.count - emitted < kQuadsPerBatch
                        ? state.count - emitted
                        : kQuadsPerBatch;
                const uint32_t verts = quads * 6;
                gfx::Qword* tag = cursor;
                // GIF tag: NREG=3 (ST, RGBAQ, XYZ2), PRE, prim = tri | IIP
                // | ABE | TME when textured. NLOOP = verts.
                uint64_t prim = 3ull | (1ull << 3) | (1ull << 6);
                if (textured) {
                    prim |= 1ull << 4;
                }
                tag[0].lo = (static_cast<uint64_t>(verts) & 0x7FFFull) |
                            (1ull << 15) | (1ull << 46) |
                            ((prim & 0x7FFull) << 47) | (3ull << 60);
                tag[0].hi = 0x512ull;
                tag[1].lo = verts;
                tag[1].hi = 0;
                gfx::Qword* v = tag + 2;
                for (uint32_t q = 0; q < quads; ++q) {
                    const Particle& p = state.particles[emitted + q];
                    Vec3 centre = p.pos;
                    if (!world_space) {
                        centre = Vec3{model.m[0] * p.pos.x +
                                          model.m[4] * p.pos.y +
                                          model.m[8] * p.pos.z + model.m[12],
                                      model.m[1] * p.pos.x +
                                          model.m[5] * p.pos.y +
                                          model.m[9] * p.pos.z + model.m[13],
                                      model.m[2] * p.pos.x +
                                          model.m[6] * p.pos.y +
                                          model.m[10] * p.pos.z + model.m[14]};
                    }
                    const float t = 1.0f - p.life / p.ttl; // 0 birth, 1 death
                    const float size =
                        (emitter.size_start +
                         (emitter.size_end - emitter.size_start) * t) *
                        0.5f;
                    const uint32_t c0 = emitter.colour_start;
                    const uint32_t c1 = emitter.colour_end;
                    float col[4];
                    for (int ch = 0; ch < 4; ++ch) {
                        const float a =
                            static_cast<float>((c0 >> (ch * 8)) & 0xFF);
                        const float b =
                            static_cast<float>((c1 >> (ch * 8)) & 0xFF);
                        col[ch] = a + (b - a) * t;
                    }
                    // PS2 alpha: 0x80 is opaque, so halve the 0..255 ramp.
                    col[3] *= 0.5f;

                    const Vec3 rx{cam_right.x * size, cam_right.y * size,
                                  cam_right.z * size};
                    const Vec3 uy{cam_up.x * size, cam_up.y * size,
                                  cam_up.z * size};
                    const Vec3 corners[4] = {
                        {centre.x - rx.x - uy.x, centre.y - rx.y - uy.y,
                         centre.z - rx.z - uy.z}, // bottom-left
                        {centre.x + rx.x - uy.x, centre.y + rx.y - uy.y,
                         centre.z + rx.z - uy.z}, // bottom-right
                        {centre.x + rx.x + uy.x, centre.y + rx.y + uy.y,
                         centre.z + rx.z + uy.z}, // top-right
                        {centre.x - rx.x + uy.x, centre.y - rx.y + uy.y,
                         centre.z - rx.z + uy.z}, // top-left
                    };
                    // V grows DOWN in GS space: top corners get v=0.
                    static const float kU[4] = {0, 1, 1, 0};
                    static const float kV[4] = {1, 1, 0, 0};
                    static const int kTri[6] = {0, 1, 2, 0, 2, 3};
                    for (int i = 0; i < 6; ++i) {
                        const int c = kTri[i];
                        v[0] = qword4f(corners[c].x, corners[c].y,
                                              corners[c].z, 1.0f);
                        v[1] = qword4f(kU[c], kV[c], 1.0f, 0.0f);
                        v[2] = qword4f(col[0], col[1], col[2], col[3]);
                        v += 3;
                    }
                }
                batches[batch_count] = gfx::BatchBlock{
                    tag, tag + 2, verts * 3, verts, 10};
                ++batch_count;
                cursor = v;
                emitted += quads;
            }

            gfx::BatchBuilder::build_unlit_constants(viewproj.m, vscale,
                                                     voffset, 4095.0f,
                                                     cam.znear, g_constants);
            chain.begin();
            bool ok = chain.add_constants(g_constants, 7, 0);
            for (uint32_t b = 0; b < batch_count && ok; ++b) {
                ok = chain.add_batch(batches[b], programs.tex_addr);
            }
            if (!ok || !chain.kick()) {
                return false;
            }
            chain.wait(); // the scratch is reused by the next system
            local.drawn += 1;
            local.kicks += 1;
        }
        device.set_material_state(0, 0, false, true); // restore opaque
    }

    // --- uGUI canvas (M12.5 task 5) -----------------------------------------
    //
    // A SECOND frame packet, after every 3D kick has completed, so the
    // canvas is genuinely on top -- the M8 debug overlay rides the CLEAR
    // packet and 3D draws over it, which is fine for a stats readout and
    // wrong for a menu. Elements draw in table order, which the exporter
    // wrote in hierarchy order: painter's algorithm, exactly like uGUI.
    if (m_ui_ready && world.ui_element_count() > 0) {
        device.begin_frame();
        for (uint32_t i = 0; i < world.ui_element_count(); ++i) {
            const UIElement& ui = world.ui_element(i);
            if (!ui.visible || ui.w <= 0.0f || ui.h <= 0.0f) {
                continue;
            }
            const uint8_t r = static_cast<uint8_t>(ui.colour & 0xFFu);
            const uint8_t g = static_cast<uint8_t>((ui.colour >> 8) & 0xFFu);
            const uint8_t b = static_cast<uint8_t>((ui.colour >> 16) & 0xFFu);
            const uint8_t a = static_cast<uint8_t>((ui.colour >> 24) & 0xFFu);
            const int32_t x = static_cast<int32_t>(ui.x);
            const int32_t y = static_cast<int32_t>(ui.y);
            const int32_t w = static_cast<int32_t>(ui.w);
            const int32_t h = static_cast<int32_t>(ui.h);
            // kind's low byte is the DRAW kind; bits 8-15 carry the managed
            // class (Image/RawImage/Text) for the bridge, so an unmasked
            // switch would send Text (0x202) to the default rect case.
            switch (ui.kind & 0xFFu) {
                case 1: { // image
                    // The UV span must be the texture's REAL size: a 256
                    // guess over a 32px sprite tiles it eight times (the
                    // first Canvas drew a row of blobs; verify-log M12.5).
                    uint32_t tw = 0, th = 0;
                    if (ui.texture != 0xFFFFFFFFu && bind_texture != nullptr &&
                        bind_texture(bind_user, ui.texture, &tw, &th) &&
                        tw > 0 && th > 0) {
                        // Image records carry the sprite's 9-slice borders
                        // as four f32s in the text bytes (L, T, R, B; zeros
                        // for Image.Type.Simple). Corners keep their pixel
                        // size instead of stretching -- rounded UI sprites
                        // look broken without this.
                        float bl, bt, br2, bb;
                        memcpy(&bl, ui.text + 0, 4);
                        memcpy(&bt, ui.text + 4, 4);
                        memcpy(&br2, ui.text + 8, 4);
                        memcpy(&bb, ui.text + 12, 4);
                        if (bl > 0.0f || bt > 0.0f || br2 > 0.0f ||
                            bb > 0.0f) {
                            m_ui_overlay.textured_rect_sliced(
                                device, x, y, w, h, tw, th, bl, bt, br2, bb,
                                r, g, b, a);
                        } else {
                            m_ui_overlay.textured_rect(device, x, y, w, h, tw,
                                                       th, r, g, b, a);
                        }
                        break;
                    }
                    // An image with no texture is Unity's white sprite, and
                    // a FAILED bind falls back the same way: a tinted rect.
                    m_ui_overlay.fill_rect(device, x, y, w, h, r, g, b, a);
                    break;
                }
                case 2: { // text
                    // A baked Unity font when the element names one and its
                    // atlas binds; the builtin 8x8 otherwise. The fallback
                    // matters: a font whose texture did not fit in VRAM
                    // degrades to readable, not to invisible.
                    bool drew_baked = false;
                    if (ui.font >= 0 &&
                        static_cast<uint32_t>(ui.font) <
                            world.ui_font_count() &&
                        bind_texture != nullptr) {
                        const gfx::UIFont& font =
                            world.ui_font(static_cast<uint32_t>(ui.font));
                        uint32_t tw = 0, th = 0;
                        if (font.texture != 0xFFFFFFFFu &&
                            bind_texture(bind_user, font.texture, &tw, &th)) {
                            m_ui_overlay.draw_text_font(
                                device, font, x, y, w, h, ui.align_h,
                                ui.align_v, r, g, b, a, ui.text);
                            drew_baked = true;
                        }
                    }
                    if (!drew_baked) {
                        m_ui_overlay.set_colour(r, g, b);
                        m_ui_overlay.set_scale(ui.text_scale);
                        m_ui_overlay.draw_text_aligned(device, x, y, w, h,
                                                       ui.align_h, ui.align_v,
                                                       ui.text);
                    }
                    break;
                }
                default: // rect
                    m_ui_overlay.fill_rect(device, x, y, w, h, r, g, b, a);
                    break;
            }
        }
        device.end_frame(/*flip=*/false);
    }

    if (stats != nullptr) {
        *stats = local;
    }
    // Every kick has been waited on: the buffer is complete. Show it.
    device.present();
    return true;
}

} // namespace scene
} // namespace ps2ur
