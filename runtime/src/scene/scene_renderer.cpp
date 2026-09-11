#include "ps2ur/scene_renderer.h"
#include "ps2ur/phys.h"

#include "ps2ur/gs_batch.h"
#include "ps2ur/log.h"
#include "ps2ur/profiler.h"
#include "ps2ur/profiler_overlay.h"

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

// The console's bloom for a bloomed Text (PS2BootGlowText's UNITY_PS2
// path): the glyph run drawn again in a ring around itself, additively at
// a fraction of the element's alpha, so overlapping passes stack towards
// white -- the era's way of faking a glow, and cheap on the GS. Two rings
// of eight: the outer at glow_spread, the inner at under half of it,
// widened by glow_dilate. The crisp run goes on top, drawn by the caller.
static void draw_text_glow(gfx::GsDevice& device, gfx::DebugOverlay& overlay,
                           const gfx::UIFont& font, const UIElement& ui,
                           int32_t x, int32_t y, int32_t w, int32_t h,
                           uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    static const int8_t kDir[8][2] = {{1, 0},  {1, 1},   {0, 1},  {-1, 1},
                                      {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
    const float outer = ui.glow_spread;
    const float inner = ui.glow_spread * (0.35f + 0.25f * ui.glow_dilate);
    // Sixteen passes at full intensity sum to about the element's own
    // alpha where they all overlap; no single pass is anywhere near opaque.
    const float per_pass = static_cast<float>(a) * ui.glow_intensity * 0.18f;
    const uint8_t pa = static_cast<uint8_t>(per_pass > 127.0f ? 127.0f : per_pass);
    if (pa == 0u) {
        return;
    }
    overlay.set_additive(true);
    for (int ring = 0; ring < 2; ++ring) {
        const float radius = ring == 0 ? outer : inner;
        for (int d = 0; d < 8; ++d) {
            const bool diagonal = kDir[d][0] != 0 && kDir[d][1] != 0;
            const float len = diagonal ? radius * 0.7071f : radius;
            const int32_t dx = static_cast<int32_t>(static_cast<float>(kDir[d][0]) * len);
            const int32_t dy = static_cast<int32_t>(static_cast<float>(kDir[d][1]) * len);
            overlay.draw_text_font(device, font, x + dx, y + dy, w, h, ui.align_h,
                                   ui.align_v, r, g, b, pa, ui.text);
        }
    }
    overlay.set_additive(false);
}

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

// ---- M14 lighting ----------------------------------------------------------
//
// The VU programs take THREE lights per batch as constants (directions as
// the columns of one matrix, then a colour each) plus an ambient. Which
// three is decided here, per object, every frame: every enabled Light in
// the scene is scored against the object's bounding sphere and the
// brightest three win. A directional light contributes its entity's
// forward axis. A point light contributes the direction from itself to the
// object's centre with a (1 - d/reach)^2 falloff, a spot light the same
// inside its cone -- the per-object approximation the era used, since
// nothing per-vertex is affordable beyond what the microprograms do.
// Directions are read from the light entities' world matrices, so a light
// parented to a moving thing, or one a script rotates, follows.

struct LightPick {
    Vec3 dir{0, 0, 1}; // world space, pointing FROM the light
    Vec3 colour{0, 0, 0};
    float weight = 0.0f;
};

static uint32_t pick_lights(const World& world, Vec3 centre, float radius,
                            LightPick* out)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < world.light_count(); ++i) {
        const Light& l = world.light_at(i);
        if (!l.enabled || l.entity < 0 ||
            !world.entity(static_cast<uint32_t>(l.entity)).alive ||
            !world.entity_visible(l.entity)) {
            continue;
        }
        const Mat4& lw = world.world_matrix(static_cast<uint32_t>(l.entity));
        // Column-major: column 2 is the entity's +Z, column 3 its position.
        Vec3 forward = normalize(Vec3{lw.m[8], lw.m[9], lw.m[10]});
        if (length_sq(forward) < 1e-6f) {
            forward = Vec3{0, 0, 1};
        }
        LightPick pick;
        float att = 1.0f;
        if (l.kind == 0u) {
            pick.dir = forward;
        } else {
            const Vec3 pos{lw.m[12], lw.m[13], lw.m[14]};
            const Vec3 to = sub(centre, pos);
            const float d = length(to);
            const float reach = l.range + radius;
            if (l.range <= 0.0f || d >= reach) {
                continue;
            }
            pick.dir = d > 1e-4f ? scale(to, 1.0f / d) : forward;
            const float t = d / reach;
            att = (1.0f - t) * (1.0f - t);
            if (l.kind == 2u) {
                const float c = dot(pick.dir, forward);
                if (c <= l.spot_cos) {
                    continue;
                }
                const float span = 1.0f - l.spot_cos;
                att *= span > 1e-4f ? (c - l.spot_cos) / span : 1.0f;
            }
        }
        pick.colour = scale(l.colour, att);
        pick.weight = pick.colour.x * 0.30f + pick.colour.y * 0.59f + pick.colour.z * 0.11f;
        if (pick.weight <= 0.002f) {
            continue;
        }
        // Insert by weight, keeping the best three.
        uint32_t slot = n < 3u ? n : 3u;
        while (slot > 0u && out[slot - 1u].weight < pick.weight) {
            if (slot < 3u) {
                out[slot] = out[slot - 1u];
            }
            --slot;
        }
        if (slot < 3u) {
            out[slot] = pick;
            if (n < 3u) {
                ++n;
            }
        }
    }
    return n;
}

// Fills constant qwords 7..16 for a lit program: the three light slots in
// OBJECT space (the microprograms light against untransformed normals),
// their colours, the ambient and the colour clamp.
static void fill_light_constants(const World& world, const Mat4& w, Vec3 centre,
                                 float radius, gfx::Qword* constants)
{
    LightPick picks[3];
    const uint32_t n = pick_lights(world, centre, radius, picks);
    float dx[3] = {0, 0, 0}, dy[3] = {0, 0, 0}, dz[3] = {0, 0, 0};
    for (uint32_t i = 0; i < n; ++i) {
        const Vec3 ld = picks[i].dir;
        // Row i of the rotation is column i of the column-major matrix:
        // this is R^T * (-ld), the light direction TOWARDS the light in the
        // object's own frame, which is what N.L wants.
        const Vec3 obj = normalize(Vec3{
            -(w.m[0] * ld.x + w.m[1] * ld.y + w.m[2] * ld.z),
            -(w.m[4] * ld.x + w.m[5] * ld.y + w.m[6] * ld.z),
            -(w.m[8] * ld.x + w.m[9] * ld.y + w.m[10] * ld.z)});
        dx[i] = obj.x;
        dy[i] = obj.y;
        dz[i] = obj.z;
    }
    set_float4(constants[7], 0, 0, 0, 0);
    set_float4(constants[8], 0, 0, 0, 0);
    set_float4(constants[9], dx[0], dx[1], dx[2], 0);
    set_float4(constants[10], dy[0], dy[1], dy[2], 0);
    set_float4(constants[11], dz[0], dz[1], dz[2], 0);
    for (uint32_t i = 0; i < 3u; ++i) {
        const Vec3 c = i < n ? picks[i].colour : Vec3{0, 0, 0};
        // Scale discipline: exported vertex colours are 0..255, so the
        // light factor stays ~0..1 (verify-log M8).
        set_float4(constants[12 + i], c.x, c.y, c.z, 0);
    }
    const Vec3 amb = world.camera().ambient;
    set_float4(constants[15], amb.x, amb.y, amb.z, 0);
    set_float4(constants[16], 255.0f, 255.0f, 255.0f, 128.0f);
}

// Lights off: what a projected shadow draws with. Every light slot zero and
// the ambient zero, so a lit program outputs black whatever the vertex
// colours are, and the GS blend darkens the ground by the shadow's FIX.
static void zero_light_constants(gfx::Qword* constants)
{
    for (uint32_t i = 7; i <= 15; ++i) {
        set_float4(constants[i], 0, 0, 0, 0);
    }
    set_float4(constants[16], 255.0f, 255.0f, 255.0f, 128.0f);
}

// LODGroup levels (M14): Unity's relative screen height, size / distance
// over the view's vertical extent, against the level's [min, max).
static bool lod_visible(const World& world, uint32_t entity, Vec3 cam_pos,
                        float view_extent_per_unit, bool orthographic,
                        float ortho_size)
{
    const Entity& ent = world.entity(entity);
    if (ent.lod < 0) {
        return true;
    }
    const LodRef& lod = world.lod(static_cast<uint32_t>(ent.lod));
    const Mat4& w = world.world_matrix(entity);
    float rel;
    if (orthographic) {
        rel = ortho_size > 0.0f ? lod.size / (2.0f * ortho_size) : 1.0f;
    } else {
        const float d = length(sub(Vec3{w.m[12], w.m[13], w.m[14]}, cam_pos));
        rel = d > 1e-3f ? lod.size / (d * view_extent_per_unit) : 10.0f;
    }
    return rel >= lod.min_height && rel < lod.max_height;
}

// The planar projection along light direction L onto the plane through P
// with normal N (M14 projected shadows): x' = x - L * (N.x - N.P) / (N.L).
// Column-major, like every Mat4 here.
static Mat4 shadow_projection(Vec3 p, Vec3 n, Vec3 l)
{
    const float k = dot(n, l);
    Mat4 s = mat4_identity();
    const float ln[3] = {l.x, l.y, l.z};
    const float nn[3] = {n.x, n.y, n.z};
    for (uint32_t c = 0; c < 3; ++c) {
        for (uint32_t r = 0; r < 3; ++r) {
            s.m[c * 4 + r] = (r == c ? 1.0f : 0.0f) - ln[r] * nn[c] / k;
        }
    }
    const float np = dot(n, p) / k;
    s.m[12] = l.x * np;
    s.m[13] = l.y * np;
    s.m[14] = l.z * np;
    s.m[15] = 1.0f;
    return s;
}

// One skinned character through the chain: palette, batches, kick. Shared
// by the skinned pass and the projected-shadow pass (M14), which draws the
// same character through the shadow matrix with the lights off.
struct SkinDraw {
    gfx::GsDevice* device;
    gfx::DmaChain* chain;
    const World* world;
    const RendererPrograms* programs;
    SceneRenderer::BindTextureFn bind_texture;
    void* bind_user;
    const Mat4* viewproj;
    float* vscale;
    float* voffset;
    float znear;
    RenderStats* stats;
};

static bool draw_skinned(const SkinDraw& a, const SkinnedRenderer& renderer,
                         const Mat4& w, Vec3 centre, float radius, bool lights_off)
{
    const World& world = *a.world;
    const LoadedSkinnedMesh& mesh =
        world.skinned_mesh(static_cast<uint32_t>(renderer.mesh));
    const anim::Animator& animator = world.animator(renderer.animator);

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
    if (mesh.textured && a.bind_texture != nullptr && skin_mat >= 0 &&
        static_cast<uint32_t>(skin_mat) < world.material_count()) {
        const LoadedMaterial& sm = world.material(static_cast<uint32_t>(skin_mat));
        if (sm.texture_index != 0xFFFFFFFFu) {
            uint32_t tw = 0, th = 0;
            a.device->packet().reset();
            if (a.bind_texture(a.bind_user, sm.texture_index, &tw, &th)) {
                a.device->flush_packet();
            }
        }
    }
    const uint32_t skin_program =
        mesh.textured ? a.programs->skin_tex_addr : a.programs->skin_addr;

    const Mat4 mvp = mat4_mul(*a.viewproj, w);
    gfx::BatchBuilder::build_unlit_constants(mvp.m, a.vscale, a.voffset, 4095.0f,
                                             a.znear, g_constants);
    if (lights_off) {
        zero_light_constants(g_constants);
    } else {
        fill_light_constants(world, w, centre, radius, g_constants);
    }
    set_float4(g_constants[17], 0, 0, 0, 0);

    uint32_t last_table = 0xFFFFFFFFu;
    a.chain->begin();
    bool ok = true;
    for (uint32_t b = 0; b < mesh.batch_count && ok; ++b) {
        const uint32_t table_count = mesh.bone_count[b];
        // Re-upload the palette only when this batch's bone table differs
        // from the one already resident.
        if (last_table == 0xFFFFFFFFu ||
            mesh.bone_table[b][0] != mesh.bone_table[last_table][0] ||
            table_count != mesh.bone_count[last_table]) {
            anim::build_palette(animator, mesh.bone_table[b], table_count, g_palette);
            for (uint32_t slot = 0; slot < table_count; ++slot) {
                for (uint32_t c = 0; c < 4; ++c) {
                    set_float4(g_constants[18u + slot * 4u + c],
                               g_palette[slot].m[c * 4 + 0], g_palette[slot].m[c * 4 + 1],
                               g_palette[slot].m[c * 4 + 2], g_palette[slot].m[c * 4 + 3]);
                }
            }
            ok = a.chain->add_constants(g_constants, 18u + table_count * 4u, 0);
            last_table = b;
        }
        if (ok) {
            ok = a.chain->add_batch(mesh.batches[b], skin_program);
            ++a.stats->skin_batches;
        }
    }
    if (!ok || !a.chain->kick()) {
        return false;
    }
    a.chain->wait();
    ++a.stats->kicks;
    ++a.stats->skinned_drawn;
    return true;
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
    // The camera's world position: a follow-camera entity (the sky) draws
    // with its translation replaced by this, so it can never be approached.
    const Mat4& cam_world = world.world_matrix(cam_entity);
    const Vec3 cam_pos{cam_world.m[12], cam_world.m[13], cam_world.m[14]};
    // LOD: the view's vertical extent per unit of distance (M14).
    const float view_extent_per_unit = 2.0f * __builtin_tanf(cam.fov * 0.5f);
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
    //
    // Zoned separately because plan section 9 M13 task 3 lists MMI/SIMD for
    // "the transform update and culling" as an optimisation candidate. That
    // is a claim about where the time goes, and it needs a number before
    // anyone rewrites a loop in vector intrinsics.
    const uint32_t zone_cull = prof::zone_id("cull");
    prof::zone_begin(zone_cull);
    const bool dbg = m_debug_frame;
    m_debug_frame = false;
    m_queue.clear();
    const float inv_depth_range = 1.0f / (cam.zfar - cam.znear);
    for (uint32_t e = 0; e < world.entity_count(); ++e) {
        const Entity& ent = world.entity(e);
        if (!ent.alive || ent.mesh < 0) {
            continue;
        }
        if (!world.entity_visible(static_cast<int32_t>(e))) {
            if (dbg) {
                log(LogLevel::Info, "rdbg: e%u mesh %d INACTIVE",
                    static_cast<unsigned>(e), static_cast<int>(ent.mesh));
            }
            continue;
        }
        ++local.considered;
        if (((1u << (ent.layer & 31u)) & cam.layer_mask) == 0u) {
            ++local.culled;
            if (dbg) {
                log(LogLevel::Info, "rdbg: e%u mesh %d LAYER %u masked",
                    static_cast<unsigned>(e), static_cast<int>(ent.mesh),
                    static_cast<unsigned>(ent.layer));
            }
            continue;
        }
        if (!lod_visible(world, e, cam_pos, view_extent_per_unit, cam.orthographic,
                         cam.ortho_size)) {
            ++local.culled;
            continue;
        }
        const LoadedMesh& mesh = world.mesh(static_cast<uint32_t>(ent.mesh));
        const uint32_t mat_index =
            ent.material >= 0 ? static_cast<uint32_t>(ent.material)
                              : mesh.material_index;
        const LoadedMaterial& mat = world.material(mat_index);

        Mat4 follow;
        const Mat4* wp = &world.world_matrix(e);
        if (ent.follow_camera) {
            follow = *wp;
            follow.m[12] = cam_pos.x;
            follow.m[13] = cam_pos.y;
            follow.m[14] = cam_pos.z;
            wp = &follow;
        }
        const Mat4& w = *wp;
        const Vec4 c = mat4_mul_vec4(
            w, Vec4{mesh.bounds_center.x, mesh.bounds_center.y,
                    mesh.bounds_center.z, 1.0f});
        const float radius = world_radius(w, mesh.bounds_radius);
        if (!ent.follow_camera &&
            frustum_culls_sphere(frustum, Vec3{c.x, c.y, c.z}, radius)) {
            ++local.culled;
            if (dbg) {
                log(LogLevel::Info,
                    "rdbg: e%u mesh %d FRUSTUM-CULLED c=(%d,%d,%d) r=%d",
                    static_cast<unsigned>(e), static_cast<int>(ent.mesh),
                    static_cast<int>(c.x), static_cast<int>(c.y),
                    static_cast<int>(c.z), static_cast<int>(radius));
            }
            continue;
        }
        if (dbg) {
            log(LogLevel::Info,
                "rdbg: e%u mesh %d mat %u kind %u tex %d QUEUED",
                static_cast<unsigned>(e), static_cast<int>(ent.mesh),
                static_cast<unsigned>(mat_index),
                static_cast<unsigned>(mat.kind),
                mat.texture_index == 0xFFFFFFFFu
                    ? -1
                    : static_cast<int>(mat.texture_index));
        }

        const Vec4 vz = mat4_mul_vec4(flipped_view, c);
        const float depth01 = (vz.z - cam.znear) * inv_depth_range;
        const uint32_t pass = mat.sky ? 0u : mat.transparent ? 2u : 1u;
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
    prof::zone_end(zone_cull);

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
            bool bound = false;
            if (tex1 != 0u && bind_texture != nullptr) {
                uint32_t tw = 0, th = 0;
                bound = bind_texture(bind_user, mat.texture_index, &tw, &th);
                device.set_texture_clamp(mat.clamp);
            }
            device.flush_packet();
            if (dbg) {
                log(LogLevel::Info,
                    "rdbg: group mat %u kind %u tex %d bind=%d",
                    static_cast<unsigned>(cmd.material),
                    static_cast<unsigned>(mat.kind),
                    tex1 == 0u ? -1 : static_cast<int>(mat.texture_index),
                    bound ? 1 : 0);
            }
            current_group = group;
        }
        if (!chain_open) {
            chain.begin();
            chain_open = true;
        }

        Mat4 follow;
        const Mat4* wp = &world.world_matrix(cmd.entity);
        if (world.entity(cmd.entity).follow_camera) {
            follow = *wp;
            follow.m[12] = cam_pos.x;
            follow.m[13] = cam_pos.y;
            follow.m[14] = cam_pos.z;
            wp = &follow;
        }
        const Mat4 mvp = mat4_mul(viewproj, *wp);
        bool ok;
        if (kind_uses_lit_constants(mat.kind)) {
            const Mat4& w = *wp;
            const Vec4 lc4 = mat4_mul_vec4(
                w, Vec4{mesh.bounds_center.x, mesh.bounds_center.y,
                        mesh.bounds_center.z, 1.0f});
            gfx::BatchBuilder::build_unlit_constants(mvp.m, vscale, voffset,
                                                     4095.0f, cam.znear,
                                                     g_constants);
            fill_light_constants(world, w, Vec3{lc4.x, lc4.y, lc4.z},
                                 world_radius(w, mesh.bounds_radius), g_constants);
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
        PS2UR_PROFILE_ZONE("skinned");
        device.packet().reset();
        device.set_material_state(0, 0, false, true);
        device.flush_packet();
    }
    SkinDraw skin_args{&device, &chain, &world, &programs, bind_texture, bind_user,
                       &viewproj, vscale, voffset, cam.znear, &local};
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
        if (!lod_visible(world, entity, cam_pos, view_extent_per_unit,
                         cam.orthographic, cam.ortho_size)) {
            ++local.culled;
            continue;
        }
        const LoadedSkinnedMesh& mesh =
            world.skinned_mesh(static_cast<uint32_t>(renderer.mesh));

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
        if (!draw_skinned(skin_args, renderer, w, Vec3{centre.x, centre.y, centre.z},
                          radius, false)) {
            return false;
        }
        chain_open = false;
    }

    // --- Shadows (M14) ------------------------------------------------------
    //
    // Two techniques, both the era's. A BLOB: a fan of black triangles on
    // the ground under the caster, alpha 1 at the centre and 0 at the rim,
    // faded by height, found by a raycast straight down. PROJECTED (mode
    // 1): the caster's own lit meshes and skinned renderers drawn again
    // through a planar projection along the strongest directional light
    // onto the ground plane the raycast found, with every light off so
    // they come out black, blended as Cd * (1 - strength) through the GS
    // FIX alpha. No Z write, depth-tested, lifted a little off the plane.
    // Unlit and textured-unlit meshes cannot be turned black by constants,
    // so they cast blobs only.
    if (world.shadow_count() > 0) {
        PS2UR_PROFILE_ZONE("shadows");
        Vec3 sun{0, -1, 0};
        float sun_weight = -1.0f;
        for (uint32_t i = 0; i < world.light_count(); ++i) {
            const Light& l = world.light_at(i);
            if (!l.enabled || l.kind != 0u || l.entity < 0 ||
                !world.entity(static_cast<uint32_t>(l.entity)).alive) {
                continue;
            }
            const float wgt = l.colour.x * 0.30f + l.colour.y * 0.59f + l.colour.z * 0.11f;
            if (wgt > sun_weight) {
                const Mat4& lw = world.world_matrix(static_cast<uint32_t>(l.entity));
                const Vec3 f = normalize(Vec3{lw.m[8], lw.m[9], lw.m[10]});
                if (length_sq(f) > 1e-6f) {
                    sun = f;
                    sun_weight = wgt;
                }
            }
        }
        // 16-segment fan: 48 vertices, 2 qwords each, after the 2-qword header.
        constexpr uint32_t kSegments = 16;
        alignas(16) static gfx::Qword blob[2 + kSegments * 3 * 2];
        static const float kCos[kSegments] = {
            1.0f, 0.92388f, 0.70711f, 0.38268f, 0.0f, -0.38268f, -0.70711f, -0.92388f,
            -1.0f, -0.92388f, -0.70711f, -0.38268f, 0.0f, 0.38268f, 0.70711f, 0.92388f};
        static const float kSin[kSegments] = {
            0.0f, 0.38268f, 0.70711f, 0.92388f, 1.0f, 0.92388f, 0.70711f, 0.38268f,
            0.0f, -0.38268f, -0.70711f, -0.92388f, -1.0f, -0.92388f, -0.70711f, -0.38268f};

        for (uint32_t si = 0; si < world.shadow_count(); ++si) {
            const ShadowRef& sh = world.shadow(si);
            if (sh.entity < 0 || !world.entity(static_cast<uint32_t>(sh.entity)).alive ||
                !world.entity_visible(sh.entity)) {
                continue;
            }
            const uint32_t caster = static_cast<uint32_t>(sh.entity);
            const Mat4& cw = world.world_matrix(caster);
            const Vec3 origin{cw.m[12], cw.m[13] + 0.1f, cw.m[14]};
            phys::RaycastHit hit;
            if (!phys::raycast(origin, Vec3{0, -1, 0}, sh.max_height + 1.0f,
                               phys::kAllLayers, &hit)) {
                continue;
            }
            float height = hit.distance - 0.1f;
            if (height < 0.0f) {
                height = 0.0f;
            }
            const float fade = sh.max_height > 0.0f ? 1.0f - height / sh.max_height : 1.0f;
            if (fade <= 0.0f || sh.strength <= 0.0f) {
                continue;
            }
            Vec3 n = normalize(hit.normal);
            if (length_sq(n) < 1e-6f) {
                n = Vec3{0, 1, 0};
            }
            const Vec3 p = add(hit.point, scale(n, 0.02f));
            // A frustum test on the ground point keeps off-screen casters
            // from spending a kick.
            if (frustum_culls_sphere(frustum, p, sh.radius * 2.0f + 1.0f)) {
                continue;
            }

            // Blob fan in the plane's own basis.
            Vec3 t = __builtin_fabsf(n.y) < 0.9f ? cross(n, Vec3{0, 1, 0})
                                                 : cross(n, Vec3{1, 0, 0});
            t = normalize(t);
            const Vec3 bt = cross(n, t);
            const float alpha = sh.strength * fade * 128.0f;
            const uint32_t verts = kSegments * 3;
            // GIF tag: NREG=2 (RGBAQ, XYZ2), PRE, prim = tri | IIP | ABE.
            const uint64_t prim = 3ull | (1ull << 3) | (1ull << 6);
            blob[0].lo = (static_cast<uint64_t>(verts) & 0x7FFFull) | (1ull << 15) |
                         (1ull << 46) | ((prim & 0x7FFull) << 47) | (2ull << 60);
            blob[0].hi = 0x51ull;
            blob[1].lo = verts;
            blob[1].hi = 0;
            gfx::Qword* v = blob + 2;
            for (uint32_t k = 0; k < kSegments; ++k) {
                const uint32_t k1 = (k + 1u) % kSegments;
                const Vec3 r0 = add(p, add(scale(t, kCos[k] * sh.radius),
                                           scale(bt, kSin[k] * sh.radius)));
                const Vec3 r1 = add(p, add(scale(t, kCos[k1] * sh.radius),
                                           scale(bt, kSin[k1] * sh.radius)));
                v[0] = qword4f(p.x, p.y, p.z, 1.0f);
                v[1] = qword4f(0, 0, 0, alpha);
                v[2] = qword4f(r0.x, r0.y, r0.z, 1.0f);
                v[3] = qword4f(0, 0, 0, 0);
                v[4] = qword4f(r1.x, r1.y, r1.z, 1.0f);
                v[5] = qword4f(0, 0, 0, 0);
                v += 6;
            }
            gfx::BatchBlock blob_block{blob, blob + 2, verts * 2u, verts, 10u};

            device.packet().reset();
            // (Cs - Cd) * As + Cd with Cs black: the ground darkens by the
            // vertex alpha. No Z write; depth-tested against the ground.
            device.set_material_state(0, 0x44u, true, false);
            device.flush_packet();
            gfx::BatchBuilder::build_unlit_constants(viewproj.m, vscale, voffset,
                                                     4095.0f, cam.znear, g_constants);
            chain.begin();
            bool ok = chain.add_constants(g_constants, 7, 0) &&
                      chain.add_batch(blob_block, programs.unlit_addr);
            if (!ok || !chain.kick()) {
                return false;
            }
            chain.wait();
            ++local.kicks;
            ++local.drawn;

            if (sh.mode != 1u || dot(n, sun) > -0.05f) {
                continue;
            }
            const Mat4 proj = shadow_projection(p, n, sun);
            uint8_t fix = static_cast<uint8_t>(sh.strength * fade * 128.0f);
            if (fix > 128u) {
                fix = 128u;
            }
            // (Cs - Cd) * FIX + Cd, Cs black: Cd * (1 - strength).
            const uint64_t shadow_alpha = gfx::gs_alpha(0, 1, 2, 1, fix);

            // The caster's rigid lit meshes, and its descendants' (chunks
            // and submeshes ride on synthetic children).
            for (uint32_t e = 0; e < world.entity_count(); ++e) {
                const Entity& ent = world.entity(e);
                if (!ent.alive || ent.mesh < 0 ||
                    (e != caster && !world.is_descendant_of(static_cast<int32_t>(e),
                                                            static_cast<int32_t>(caster))) ||
                    !world.entity_visible(static_cast<int32_t>(e))) {
                    continue;
                }
                const LoadedMesh& mesh = world.mesh(static_cast<uint32_t>(ent.mesh));
                const uint32_t mi = ent.material >= 0 ? static_cast<uint32_t>(ent.material)
                                                      : mesh.material_index;
                const LoadedMaterial& mat = world.material(mi);
                if (!kind_uses_lit_constants(mat.kind)) {
                    continue;
                }
                const Mat4 sw = mat4_mul(proj, world.world_matrix(e));
                const Mat4 mvp = mat4_mul(viewproj, sw);
                device.packet().reset();
                device.set_material_state(0, shadow_alpha, true, false);
                device.flush_packet();
                gfx::BatchBuilder::build_unlit_constants(mvp.m, vscale, voffset, 4095.0f,
                                                         cam.znear, g_constants);
                zero_light_constants(g_constants);
                chain.begin();
                ok = chain.add_constants(g_constants, 17, 0);
                for (uint32_t b = 0; b < mesh.batch_count && ok; ++b) {
                    ok = chain.add_batch(mesh.batches[b], programs.lit_addr);
                }
                if (!ok || !chain.kick()) {
                    return false;
                }
                chain.wait();
                ++local.kicks;
                ++local.drawn;
            }
            // The caster's skinned renderers, through the same projection.
            for (uint32_t s = 0; s < world.skinned_renderer_count(); ++s) {
                const SkinnedRenderer& renderer = world.skinned_renderer(s);
                if (renderer.entity < 0 || renderer.mesh < 0) {
                    continue;
                }
                const uint32_t se = static_cast<uint32_t>(renderer.entity);
                if (se != caster &&
                    !world.is_descendant_of(renderer.entity, static_cast<int32_t>(caster))) {
                    continue;
                }
                if (!world.entity(se).alive || !world.entity_visible(renderer.entity)) {
                    continue;
                }
                const LoadedSkinnedMesh& smesh =
                    world.skinned_mesh(static_cast<uint32_t>(renderer.mesh));
                const Mat4& w = world.world_matrix(se);
                const Mat4 sw = mat4_mul(proj, w);
                const Vec4 centre = mat4_mul_vec4(
                    w, Vec4{smesh.bounds_center.x, smesh.bounds_center.y,
                            smesh.bounds_center.z, 1.0f});
                device.packet().reset();
                device.set_material_state(0, shadow_alpha, true, false);
                device.flush_packet();
                if (!draw_skinned(skin_args, renderer, sw,
                                  Vec3{centre.x, centre.y, centre.z},
                                  world_radius(w, smesh.bounds_radius), true)) {
                    return false;
                }
            }
        }
        device.packet().reset();
        device.set_material_state(0, 0, false, true); // restore opaque
        device.flush_packet();
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
                    // Textured quads MODULATE (0x80 = 1.0): RGB halves too,
                    // or the texture renders doubled -- the same rule the
                    // exporters follow for textured vertex colours
                    // (verify-log M12.5). Untextured quads keep 0..255:
                    // the colour IS the pixel.
                    if (textured) {
                        col[0] *= 0.5f;
                        col[1] *= 0.5f;
                        col[2] *= 0.5f;
                    }

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
    // The profiler overlay rides this same "after every 3D kick" packet
    // rather than the clear packet, for the reason the comment above gives:
    // a stats readout the scene draws over is a stats readout you cannot
    // read (M13 task 1).
    const bool draw_profiler = prof::page() != prof::Page::Off;
    if (m_ui_ready && (world.ui_element_count() > 0 || draw_profiler)) {
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
                            if (ui.glow_intensity > 0.0f && ui.glow_spread > 0.0f) {
                                draw_text_glow(device, m_ui_overlay, font, ui, x, y,
                                               w, h, r, g, b, a);
                            }
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
        if (draw_profiler) {
            m_ui_overlay.set_scale(1);
            prof::draw_overlay(device, m_ui_overlay,
                               static_cast<int32_t>(device.config().width),
                               static_cast<int32_t>(device.config().height));
        }
        device.end_frame(/*flip=*/false);
    }

    if (stats != nullptr) {
        *stats = local;
    }
    // Every kick has been waited on: the buffer is complete. Show it.
    //
    // This gets its own zone because present() blocks on vsync, and a frame
    // budget that counts the wait as work says "rendering costs 33 ms" for a
    // scene that finished in 4 and then idled. Separating it is the whole
    // difference between "we are over budget" and "we are done early"
    // (plan section 15.3).
    {
        PS2UR_PROFILE_ZONE("present");
        device.present();
    }
    return true;
}

} // namespace scene
} // namespace ps2ur
