#include "ps2ur/scene_renderer.h"

#include "ps2ur/gs_batch.h"
#include "ps2ur/log.h"

namespace ps2ur {
namespace scene {

namespace {

alignas(16) gfx::Qword g_constants[18]; // 0-16 lit block + 17 fog

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

} // namespace

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
    device.end_frame();

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
        const Entity& ent = world.entity(cmd.entity);
        const LoadedMesh& mesh = world.mesh(cmd.mesh);
        const LoadedMaterial& mat = world.material(cmd.material);
        const uint32_t tex1 =
            mat.texture_index == 0xFFFFFFFFu ? 0u : mat.texture_index + 1u;
        const uint32_t group = ((mat.transparent ? 1u : 0u) << 16) |
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
                bind_texture(bind_user, mat.texture_index);
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
            // Scale discipline: exported vertex colours are 0..255, so the
            // light factor must be ~0..1: intensity = lc*N.L + ambient, then
            // x colour -> 0..255 (matches 12-vu1-lit's verified arithmetic
            // with its unity-scale vertex colour). The old *255 here double-
            // scaled and saturated every lit pixel white (verify-log, M8).
            set_float4(g_constants[12], lc.x, 0, 0, 0);
            set_float4(g_constants[13], lc.y, 0, 0, 0);
            set_float4(g_constants[14], lc.z, 0, 0, 0);
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
    }

    if (stats != nullptr) {
        *stats = local;
    }
    return true;
}

} // namespace scene
} // namespace ps2ur
