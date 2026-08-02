// samples/02-scene-graph -- M8 acceptance: 500 objects, 5 materials, deep
// hierarchy, moving camera, sorted transparency, 30 fps, goldens from 8
// fixed camera positions (plan section 9, M8).
//
// Loads host:scenegraph.p2b (25 towers x 20 cubes in 20-deep parent chains,
// exported by Ps2.Editor.PS2ExportMenu.ExportSceneGraphScene) and renders it
// through the full M8 path: frustum culling, layer masks, radix-sorted
// render queue (opaque front-to-back, transparent back-to-front with Z-write
// off), materials as precomputed GS register data, and the overlay 2D pass
// drawing live cull/draw counts.
//
// Verification without eyes:
//  1. 300 vsync-locked frames with the camera dollying along the tower
//     field; measured fps must be >= 29.5 (NTSC ceiling is 29.97 -- never
//     test >= 30.0, verify-log).
//  2. Culling proof: the moving camera must show a frame where a meaningful
//     share of objects is culled AND a frame where most are visible.
//  3. 8 fixed camera poses -> readback -> 64 tile CRCs each, emitted as
//     GOLDEN_TILE lines with the tile x offset by pose*8 so a mismatch names
//     the pose. Transparent-vs-opaque ordering errors change these tiles.
#include <ps2ur/dma_chain.h>
#include <ps2ur/gs_device.h>
#include <ps2ur/gs_overlay.h>
#include <ps2ur/log.h>
#include <ps2ur/math.h>
#include <ps2ur/p2b.h>
#include <ps2ur/p2b_scene.h>
#include <ps2ur/platform.h>
#include <ps2ur/scene_renderer.h>
#include <ps2ur/vu_program.h>

#include <kernel.h>
#include <stdio.h>

using namespace ps2ur;

extern "C" u32 VuUnlit_CodeStart __attribute__((section(".vudata")));
extern "C" u32 VuUnlit_CodeEnd __attribute__((section(".vudata")));
extern "C" u32 VuUnlitTex_CodeStart __attribute__((section(".vudata")));
extern "C" u32 VuUnlitTex_CodeEnd __attribute__((section(".vudata")));
extern "C" u32 VuLit_CodeStart __attribute__((section(".vudata")));
extern "C" u32 VuLit_CodeEnd __attribute__((section(".vudata")));

namespace {

constexpr uint32_t kScreenW = 512;
constexpr uint32_t kScreenH = 448;

alignas(16) uint8_t g_file_arena_mem[3 * 1024 * 1024];
alignas(16) uint8_t g_pixels[256 * 256 * 4];

struct GpuTexture {
    gfx::VramAlloc tex;
    gfx::VramAlloc clut;
    uint32_t w = 0, h = 0;
};

struct BindContext {
    gfx::GsDevice* device;
    GpuTexture* textures;
};

struct OverlayContext {
    gfx::GsDevice* device;
    gfx::DebugOverlay* overlay;
    scene::RenderStats stats;
};

void bind_texture(void* user, uint32_t index)
{
    BindContext* ctx = static_cast<BindContext*>(user);
    GpuTexture& t = ctx->textures[index];
    ctx->device->set_texture_indexed(t.tex, t.w, t.h, gfx::PixelFormat::PSMT8,
                                     t.clut, 256);
}

void draw_overlay(void* user)
{
    OverlayContext* ctx = static_cast<OverlayContext*>(user);
    // The uGUI-subset primitives (M8 task 8): a translucent panel + text.
    ctx->overlay->fill_rect(*ctx->device, 8, 8, 236, 26, 10, 10, 30, 0x50);
    ctx->overlay->printf_at(*ctx->device, 12, 12, "drawn %u culled %u kicks %u",
                            static_cast<unsigned>(ctx->stats.drawn),
                            static_cast<unsigned>(ctx->stats.culled),
                            static_cast<unsigned>(ctx->stats.kicks));
}

uint32_t rd_u32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t read_cycles()
{
    uint32_t now;
    asm volatile("mfc0 %0, $9" : "=r"(now));
    return now;
}

// Deterministic camera pose: position + yaw/pitch as a quaternion.
void set_camera_pose(scene::World& world, int32_t cam_entity, Vec3 pos,
                     float yaw, float pitch)
{
    const Quat qyaw = quat_from_axis_angle(Vec3{0, 1, 0}, yaw);
    const Quat qpitch = quat_from_axis_angle(Vec3{1, 0, 0}, pitch);
    world.set_local_position(cam_entity, pos);
    world.set_local_rotation(cam_entity, quat_mul(qyaw, qpitch));
}

} // namespace

int main(void)
{
    platform::init();

    gfx::VideoConfig config;
    config.width = kScreenW;
    config.height = kScreenH;
    config.packet_qwords = 16384;

    gfx::GsDevice device;
    if (!device.init(config)) {
        printf("PS2UR_TOKEN_SG_FAIL device\n");
        SleepThread();
        return 1;
    }

    // --- Load ---------------------------------------------------------------
    Arena file_arena;
    file_arena.init(g_file_arena_mem, sizeof(g_file_arena_mem));
    uint32_t file_size = 0;
    const void* file_data =
        io::load_file("host:scenegraph.p2b", file_arena, &file_size);
    if (file_data == nullptr) {
        file_data = io::load_file("scenegraph.p2b", file_arena, &file_size);
    }
    if (file_data == nullptr) {
        printf("PS2UR_TOKEN_SG_FAIL load\n");
        SleepThread();
        return 1;
    }
    io::P2bFile file;
    if (!file.parse(file_data, file_size)) {
        printf("PS2UR_TOKEN_SG_FAIL parse: %s\n", file.error());
        SleepThread();
        return 1;
    }
    scene::World world;
    if (!world.load(file)) {
        printf("PS2UR_TOKEN_SG_FAIL world: %s\n", world.error());
        SleepThread();
        return 1;
    }
    printf("[02-scene-graph] %u entities, %u meshes, %u materials\n",
           static_cast<unsigned>(world.entity_count()),
           static_cast<unsigned>(world.mesh_count()),
           static_cast<unsigned>(world.material_count()));
    if (world.entity_count() < 500u) {
        printf("PS2UR_TOKEN_SG_FAIL not a 500-object scene\n");
        SleepThread();
        return 1;
    }

    // --- Textures -----------------------------------------------------------
    GpuTexture textures[8];
    const uint32_t tex_count = file.count_of(io::kSectionTex);
    device.begin_frame();
    device.clear(0, 0, 0);
    for (uint32_t t = 0; t < tex_count && t < 8; ++t) {
        const io::P2bSection* sec = file.find(io::kSectionTex, t);
        const uint8_t* p = sec->data;
        GpuTexture& gt = textures[t];
        gt.w = rd_u32(p + 0);
        gt.h = rd_u32(p + 4);
        gt.tex = device.vram().alloc_buffer(gt.w, gt.h, gfx::PixelFormat::PSMT8,
                                            "sg-tex");
        gt.clut = device.vram().alloc_buffer(16, 16, gfx::PixelFormat::PSMCT32,
                                             "sg-clut");
        if (!gt.tex.valid() || !gt.clut.valid() ||
            !device.upload_texture(p + 16u + 1024u, gt.tex, gt.w, gt.h,
                                   gfx::PixelFormat::PSMT8) ||
            !device.upload_clut(reinterpret_cast<const uint32_t*>(p + 16u),
                                gt.clut, 256)) {
            printf("PS2UR_TOKEN_SG_FAIL texture %u\n", static_cast<unsigned>(t));
            SleepThread();
            return 1;
        }
    }
    gfx::DebugOverlay overlay;
    if (!overlay.init(device)) {
        printf("PS2UR_TOKEN_SG_FAIL overlay\n");
        SleepThread();
        return 1;
    }
    device.end_frame();

    // --- Programs / chain / renderer ---------------------------------------
    vu::MicroProgram prog_unlit, prog_tex, prog_lit;
    prog_unlit.set_blob(&VuUnlit_CodeStart, &VuUnlit_CodeEnd, 0);
    prog_tex.set_blob(&VuUnlitTex_CodeStart, &VuUnlitTex_CodeEnd, 300);
    prog_lit.set_blob(&VuLit_CodeStart, &VuLit_CodeEnd, 700);
    if (!prog_unlit.upload() || !prog_tex.upload() || !prog_lit.upload()) {
        printf("PS2UR_TOKEN_SG_FAIL programs\n");
        SleepThread();
        return 1;
    }
    gfx::DmaChain chain;
    if (!chain.init(12288)) {
        printf("PS2UR_TOKEN_SG_FAIL chain\n");
        SleepThread();
        return 1;
    }
    scene::SceneRenderer renderer;
    scene::RendererPrograms programs; // 0 / 300 / 700 defaults

    BindContext bind_ctx{&device, textures};
    OverlayContext overlay_ctx{&device, &overlay, {}};

    if (!world.has_camera()) {
        printf("PS2UR_TOKEN_SG_FAIL no camera\n");
        SleepThread();
        return 1;
    }
    const int32_t cam_entity = world.camera().entity;

    // --- Phase 1: 300 moving-camera frames, fps + culling proof -------------
    uint32_t min_drawn = 0xFFFFFFFFu;
    uint32_t max_drawn = 0;
    const uint64_t t0 = read_cycles();
    uint64_t last = t0;
    uint64_t worst_frame_cycles = 0;
    for (uint32_t frame = 0; frame < 300; ++frame) {
        // Dolly along the field with a slow yaw sweep: starts seeing a slice
        // (most towers culled), ends overlooking everything.
        const float t = static_cast<float>(frame) / 300.0f;
        const Vec3 pos{-24.0f + 44.0f * t, 5.0f + 9.0f * t, -30.0f + 12.0f * t};
        const float yaw = -0.9f + 1.4f * t;
        const float pitch = 0.18f;
        set_camera_pose(world, cam_entity, pos, yaw, pitch);

        world.update_world_matrices();
        if (!renderer.render(device, chain, world, programs, bind_texture,
                             &bind_ctx, &overlay_ctx.stats, draw_overlay,
                             &overlay_ctx)) {
            printf("PS2UR_TOKEN_SG_FAIL render frame %u\n",
                   static_cast<unsigned>(frame));
            SleepThread();
            return 1;
        }
        if (overlay_ctx.stats.drawn < min_drawn) {
            min_drawn = overlay_ctx.stats.drawn;
        }
        if (overlay_ctx.stats.drawn > max_drawn) {
            max_drawn = overlay_ctx.stats.drawn;
        }
        const uint64_t now = read_cycles();
        if (now - last > worst_frame_cycles) {
            worst_frame_cycles = now - last;
        }
        last = now;
    }
    const uint64_t elapsed = read_cycles() - t0;
    // 147.456 MHz COP0 clock; fps x100 to keep it integer-only.
    const uint32_t fps_x100 =
        static_cast<uint32_t>((300ull * 100ull * 147456000ull) / elapsed);
    printf("[02-scene-graph] 300 frames: fps=%u.%02u worst_frame=%u us "
           "drawn min=%u max=%u\n",
           fps_x100 / 100u, fps_x100 % 100u,
           static_cast<unsigned>(worst_frame_cycles / 147u),
           static_cast<unsigned>(min_drawn), static_cast<unsigned>(max_drawn));

    // NTSC vsync ceiling is 29.97: 29.5 is the pass bar (verify-log).
    if (fps_x100 < 2950u) {
        printf("PS2UR_TOKEN_SG_FAIL fps %u.%02u\n", fps_x100 / 100u,
               fps_x100 % 100u);
        SleepThread();
        return 1;
    }
    // Culling proof: the sweep must have culled hard somewhere and seen
    // nearly everything somewhere else.
    if (!(min_drawn < 350u && max_drawn > 450u)) {
        printf("PS2UR_TOKEN_SG_FAIL culling range drawn=[%u,%u]\n",
               static_cast<unsigned>(min_drawn), static_cast<unsigned>(max_drawn));
        SleepThread();
        return 1;
    }

    // --- Phase 2: 8 fixed poses -> goldens ----------------------------------
    struct Pose {
        Vec3 pos;
        float yaw, pitch;
    };
    const Pose poses[8] = {
        {{0.0f, 6.0f, -26.0f}, 0.00f, 0.15f},   // front centre
        {{-18.0f, 4.0f, -20.0f}, -0.45f, 0.10f}, // front left
        {{18.0f, 4.0f, -20.0f}, 0.45f, 0.10f},  // front right
        {{0.0f, 24.0f, -12.0f}, 0.00f, 0.85f},  // high, looking down
        {{0.0f, 3.0f, 6.0f}, 0.00f, 0.05f},     // inside the field
        {{-26.0f, 8.0f, 16.0f}, -1.35f, 0.25f}, // far side, oblique
        {{0.0f, 2.0f, -6.0f}, 0.00f, -0.10f},   // low, through the alpha towers
        {{30.0f, 12.0f, 34.0f}, 2.60f, 0.35f},  // behind, looking back
    };
    for (uint32_t p = 0; p < 8; ++p) {
        set_camera_pose(world, cam_entity, poses[p].pos, poses[p].yaw,
                        poses[p].pitch);
        world.update_world_matrices();
        if (!renderer.render(device, chain, world, programs, bind_texture,
                             &bind_ctx, &overlay_ctx.stats, nullptr, nullptr)) {
            printf("PS2UR_TOKEN_SG_FAIL golden render %u\n",
                   static_cast<unsigned>(p));
            SleepThread();
            return 1;
        }
        if (!device.read_framebuffer(g_pixels, 128, 96, 256, 256)) {
            printf("PS2UR_TOKEN_SG_FAIL readback %u\n", static_cast<unsigned>(p));
            SleepThread();
            return 1;
        }
        if (p == 0) {
            printf("GOLDEN 256x256 at (128,96) tiles 64x8 (8 poses)\n");
        }
        for (uint32_t ty = 0; ty < 8; ++ty) {
            for (uint32_t tx = 0; tx < 8; ++tx) {
                printf("GOLDEN_TILE %u %u %08X\n",
                       static_cast<unsigned>(p * 8u + tx),
                       static_cast<unsigned>(ty),
                       static_cast<unsigned>(
                           gfx::GsDevice::tile_crc32(g_pixels, 256, 256, tx, ty)));
            }
        }
    }

    printf("PS2UR_TOKEN_SG_OK\n");
    chain.shutdown();
    device.shutdown();
    SleepThread();
    return 0;
}
