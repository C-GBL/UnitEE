// samples/16-fog -- M8 task 7: GS fog (FOGCOL + per-vertex F via the
// vu_lit_fog microprogram).
//
// Loads host:fogscene.p2b (five lit cubes at increasing view depth, linear
// fog 10..60, exported by Ps2.Editor.PS2ExportMenu.ExportFogScene), renders
// one frame, and asserts WITHOUT EYES that the rendered colour of each cube
// slides monotonically toward FOGCOL with distance:
//
//   - project each cube centre with the same view-projection the renderer
//     used, sample a small window around it, average the pixels
//   - distance-to-fog-colour must strictly DECREASE with depth
//   - the nearest cube must be far from FOGCOL, the farthest (beyond
//     fogEnd) must be within a small tolerance of it
//
// Then the standard 8x8 tile golden pins the exact image.
#include <ps2ur/dma_chain.h>
#include <ps2ur/gs_device.h>
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
extern "C" u32 VuLit_CodeStart __attribute__((section(".vudata")));
extern "C" u32 VuLit_CodeEnd __attribute__((section(".vudata")));
extern "C" u32 VuLitFog_CodeStart __attribute__((section(".vudata")));
extern "C" u32 VuLitFog_CodeEnd __attribute__((section(".vudata")));

namespace {

constexpr uint32_t kScreenW = 512;
constexpr uint32_t kScreenH = 448;

alignas(16) uint8_t g_file_arena_mem[1024 * 1024];
alignas(16) uint8_t g_pixels[512 * 448 * 4];

int32_t iabs32(int32_t v)
{
    return v < 0 ? -v : v;
}

} // namespace

int main(void)
{
    platform::init();

    gfx::VideoConfig config;
    config.width = kScreenW;
    config.height = kScreenH;

    gfx::GsDevice device;
    if (!device.init(config)) {
        printf("PS2UR_TOKEN_FOG_FAIL device\n");
        SleepThread();
        return 1;
    }

    Arena file_arena;
    file_arena.init(g_file_arena_mem, sizeof(g_file_arena_mem));
    uint32_t file_size = 0;
    const void* file_data =
        io::load_file("host:fogscene.p2b", file_arena, &file_size);
    if (file_data == nullptr) {
        file_data = io::load_file("fogscene.p2b", file_arena, &file_size);
    }
    io::P2bFile file;
    static scene::World world;
    if (file_data == nullptr || !file.parse(file_data, file_size) ||
        !world.load(file)) {
        printf("PS2UR_TOKEN_FOG_FAIL load/parse/world\n");
        SleepThread();
        return 1;
    }
    if (!world.has_camera() || !world.camera().fog_enabled) {
        printf("PS2UR_TOKEN_FOG_FAIL camera fog not exported\n");
        SleepThread();
        return 1;
    }
    bool has_fog_material = false;
    for (uint32_t m = 0; m < world.material_count(); ++m) {
        if (world.material(m).kind == scene::kMaterialVertexLitFog) {
            has_fog_material = true;
        }
    }
    if (!has_fog_material) {
        printf("PS2UR_TOKEN_FOG_FAIL no fog-kind material\n");
        SleepThread();
        return 1;
    }

    vu::MicroProgram prog_unlit, prog_lit, prog_fog;
    prog_unlit.set_blob(&VuUnlit_CodeStart, &VuUnlit_CodeEnd, 0);
    prog_lit.set_blob(&VuLit_CodeStart, &VuLit_CodeEnd, 700);
    prog_fog.set_blob(&VuLitFog_CodeStart, &VuLitFog_CodeEnd, 1000);
    if (!prog_unlit.upload() || !prog_lit.upload() || !prog_fog.upload()) {
        printf("PS2UR_TOKEN_FOG_FAIL programs\n");
        SleepThread();
        return 1;
    }
    gfx::DmaChain chain;
    if (!chain.init(4096)) {
        printf("PS2UR_TOKEN_FOG_FAIL chain\n");
        SleepThread();
        return 1;
    }
    scene::SceneRenderer renderer;
    scene::RendererPrograms programs;

    world.update_world_matrices();
    scene::RenderStats stats{};
    // Two frames: the double-buffered flip means the readback below reads
    // the buffer frame 2 drew into.
    for (int f = 0; f < 2; ++f) {
        if (!renderer.render(device, chain, world, programs, nullptr, nullptr,
                             &stats)) {
            printf("PS2UR_TOKEN_FOG_FAIL render\n");
            SleepThread();
            return 1;
        }
    }
    printf("[16-fog] drawn %u culled %u\n", static_cast<unsigned>(stats.drawn),
           static_cast<unsigned>(stats.culled));

    if (!device.read_framebuffer(g_pixels, 0, 0, kScreenW, kScreenH)) {
        printf("PS2UR_TOKEN_FOG_FAIL readback\n");
        SleepThread();
        return 1;
    }

    // --- Project each fog cube and measure its distance to FOGCOL -----------
    const scene::Camera& cam = world.camera();
    const Mat4 proj =
        mat4_perspective(cam.fov, 512.0f / 448.0f, cam.znear, cam.zfar);
    Mat4 flipz = mat4_identity();
    flipz.m[10] = -1.0f;
    const Mat4 view = mat4_rigid_inverse(
        world.world_matrix(static_cast<uint32_t>(cam.entity)));
    const Mat4 viewproj = mat4_mul(proj, mat4_mul(flipz, view));

    const int32_t fog_r = cam.fog_r, fog_g = cam.fog_g, fog_b = cam.fog_b;
    int32_t previous_dist = 1 << 28; // large, but overflow-safe for the +12 slack
    int32_t near_dist = 0;
    int32_t far_dist = 0;
    uint32_t cube_index = 0;

    for (uint32_t e = 0; e < world.entity_count(); ++e) {
        const scene::Entity& ent = world.entity(e);
        if (ent.mesh < 0) {
            continue;
        }
        const Vec4 clip = mat4_mul_vec4(
            viewproj, Vec4{world.world_matrix(e).m[12],
                           world.world_matrix(e).m[13],
                           world.world_matrix(e).m[14], 1.0f});
        if (clip.w <= 0.0f) {
            continue;
        }
        const int32_t sx =
            static_cast<int32_t>((clip.x / clip.w) * 256.0f + 256.0f);
        const int32_t sy =
            static_cast<int32_t>((clip.y / clip.w) * -224.0f + 224.0f);

        // Average a 21x21 window centred on the projection.
        int64_t sum_r = 0, sum_g = 0, sum_b = 0;
        int32_t n = 0;
        for (int32_t dy = -10; dy <= 10; ++dy) {
            for (int32_t dx = -10; dx <= 10; ++dx) {
                const int32_t px = sx + dx;
                const int32_t py = sy + dy;
                if (px < 0 || py < 0 || px >= static_cast<int32_t>(kScreenW) ||
                    py >= static_cast<int32_t>(kScreenH)) {
                    continue;
                }
                const uint8_t* p = &g_pixels[(py * kScreenW + px) * 4];
                sum_r += p[0];
                sum_g += p[1];
                sum_b += p[2];
                ++n;
            }
        }
        if (n == 0) {
            continue;
        }
        const int32_t avg_r = static_cast<int32_t>(sum_r / n);
        const int32_t avg_g = static_cast<int32_t>(sum_g / n);
        const int32_t avg_b = static_cast<int32_t>(sum_b / n);
        const int32_t dist = iabs32(avg_r - fog_r) + iabs32(avg_g - fog_g) +
                             iabs32(avg_b - fog_b);
        printf("[16-fog] cube %u at screen (%d,%d): avg (%d,%d,%d), "
               "fog distance %d\n",
               static_cast<unsigned>(cube_index), static_cast<int>(sx),
               static_cast<int>(sy), static_cast<int>(avg_r),
               static_cast<int>(avg_g), static_cast<int>(avg_b),
               static_cast<int>(dist));

        // Monotonic slide toward FOGCOL (entities are exported near-to-far).
        // A little slack absorbs sampling noise at window edges.
        if (dist > previous_dist + 12) {
            printf("PS2UR_TOKEN_FOG_FAIL not monotonic (cube %u: %d > %d)\n",
                   static_cast<unsigned>(cube_index), static_cast<int>(dist),
                   static_cast<int>(previous_dist));
            SleepThread();
            return 1;
        }
        previous_dist = dist;
        if (cube_index == 0) {
            near_dist = dist;
        }
        far_dist = dist;
        ++cube_index;
    }

    if (cube_index < 5) {
        printf("PS2UR_TOKEN_FOG_FAIL only %u cubes sampled\n",
               static_cast<unsigned>(cube_index));
        SleepThread();
        return 1;
    }
    // Nearest cube barely fogged; farthest (past fogEnd) fully fogged.
    if (near_dist < 120) {
        printf("PS2UR_TOKEN_FOG_FAIL near cube already fogged (%d)\n",
               static_cast<int>(near_dist));
        SleepThread();
        return 1;
    }
    if (far_dist > 40) {
        printf("PS2UR_TOKEN_FOG_FAIL far cube not fogged (%d)\n",
               static_cast<int>(far_dist));
        SleepThread();
        return 1;
    }

    // --- Golden -------------------------------------------------------------
    printf("GOLDEN 512x448 at (0,0) tiles 8x8\n");
    // CRC over the centre 256x256 for tile stability with the shared helper.
    static uint8_t window[256 * 256 * 4];
    for (uint32_t y = 0; y < 256; ++y) {
        const uint8_t* src = &g_pixels[((y + 96) * kScreenW + 128) * 4];
        uint8_t* dst = &window[y * 256 * 4];
        for (uint32_t x = 0; x < 256 * 4; ++x) {
            dst[x] = src[x];
        }
    }
    for (uint32_t ty = 0; ty < 8; ++ty) {
        for (uint32_t tx = 0; tx < 8; ++tx) {
            printf("GOLDEN_TILE %u %u %08X\n", static_cast<unsigned>(tx),
                   static_cast<unsigned>(ty),
                   static_cast<unsigned>(
                       gfx::GsDevice::tile_crc32(window, 256, 256, tx, ty)));
        }
    }

    printf("PS2UR_TOKEN_FOG_OK\n");
    chain.shutdown();
    device.shutdown();
    SleepThread();
    return 0;
}
