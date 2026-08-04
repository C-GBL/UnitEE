// samples/22-scene-debug -- render diagnostics for ANY exported container
// (M12.5). Loads host:debugscene.p2b through the SAME World + SceneRenderer
// path the game host uses, with the full VU program set, and asks the
// renderer to log one line per entity it culls (and why), queues, and
// binds. "Why is my mesh invisible" becomes log output instead of an
// emulator debugging session.
//
// Stage any scene as debugscene.p2b next to the ELF (run-emu-test.sh does
// the staging) and read the rdbg: lines.
#include <ps2ur/dma_chain.h>
#include <ps2ur/gs_device.h>
#include <ps2ur/log.h>
#include <ps2ur/math.h>
#include <ps2ur/p2b.h>
#include <ps2ur/p2b_scene.h>
#include <ps2ur/platform.h>
#include <ps2ur/scene_renderer.h>
#include <ps2ur/vu_program.h>

#include <fcntl.h>
#include <kernel.h>
#include <stdio.h>
#include <unistd.h>

using namespace ps2ur;

extern "C" u32 VuUnlit_CodeStart __attribute__((section(".vudata")));
extern "C" u32 VuUnlit_CodeEnd __attribute__((section(".vudata")));
extern "C" u32 VuUnlitTex_CodeStart __attribute__((section(".vudata")));
extern "C" u32 VuUnlitTex_CodeEnd __attribute__((section(".vudata")));
extern "C" u32 VuLit_CodeStart __attribute__((section(".vudata")));
extern "C" u32 VuLit_CodeEnd __attribute__((section(".vudata")));
extern "C" u32 VuLitFog_CodeStart __attribute__((section(".vudata")));
extern "C" u32 VuLitFog_CodeEnd __attribute__((section(".vudata")));
extern "C" u32 VuSkin_CodeStart __attribute__((section(".vudata")));
extern "C" u32 VuSkin_CodeEnd __attribute__((section(".vudata")));
extern "C" u32 VuSkinTex_CodeStart __attribute__((section(".vudata")));
extern "C" u32 VuSkinTex_CodeEnd __attribute__((section(".vudata")));

namespace {

alignas(16) uint8_t g_file_arena_mem[8 * 1024 * 1024];
alignas(16) uint8_t g_frame[512 * 448 * 4];

struct GpuTexture {
    gfx::VramAlloc tex;
    gfx::VramAlloc clut;
    uint32_t w = 0, h = 0;
};

struct BindContext {
    gfx::GsDevice* device;
    GpuTexture* textures;
    uint32_t count;
};

bool bind_texture(void* user, uint32_t index, uint32_t* out_w, uint32_t* out_h)
{
    BindContext* ctx = static_cast<BindContext*>(user);
    if (index >= ctx->count || !ctx->textures[index].tex.valid()) {
        return false;
    }
    GpuTexture& t = ctx->textures[index];
    ctx->device->set_texture_indexed(t.tex, t.w, t.h, gfx::PixelFormat::PSMT8,
                                     t.clut, 256);
    *out_w = t.w;
    *out_h = t.h;
    return true;
}

uint32_t rd_u32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

int main(void)
{
    platform::init();

    gfx::VideoConfig config;
    config.width = 512;
    config.height = 448;
    config.packet_qwords = 16384;

    gfx::GsDevice device;
    if (!device.init(config)) {
        printf("PS2UR_TOKEN_DBG_FAIL device\n");
        SleepThread();
        return 1;
    }

    Arena file_arena;
    file_arena.init(g_file_arena_mem, sizeof(g_file_arena_mem));
    uint32_t file_size = 0;
    const void* file_data =
        io::load_file("host:debugscene.p2b", file_arena, &file_size);
    if (file_data == nullptr) {
        file_data = io::load_file("debugscene.p2b", file_arena, &file_size);
    }
    if (file_data == nullptr) {
        printf("PS2UR_TOKEN_DBG_FAIL load\n");
        SleepThread();
        return 1;
    }
    io::P2bFile file;
    if (!file.parse(file_data, file_size)) {
        printf("PS2UR_TOKEN_DBG_FAIL parse: %s\n", file.error());
        SleepThread();
        return 1;
    }
    static scene::World world;
    if (!world.load(file)) {
        printf("PS2UR_TOKEN_DBG_FAIL world: %s\n", world.error());
        SleepThread();
        return 1;
    }
    printf("[22-scene-debug] %u entities, %u meshes, %u materials, %u "
           "textures, %u skinned\n",
           static_cast<unsigned>(world.entity_count()),
           static_cast<unsigned>(world.mesh_count()),
           static_cast<unsigned>(world.material_count()),
           static_cast<unsigned>(file.count_of(io::kSectionTex)),
           static_cast<unsigned>(world.skinned_renderer_count()));

    static GpuTexture textures[64];
    const uint32_t tex_count = file.count_of(io::kSectionTex);
    for (uint32_t t = 0; t < tex_count && t < 64; ++t) {
        device.begin_frame();
        device.clear(0, 0, 0);
        const io::P2bSection* sec = file.find(io::kSectionTex, t);
        const uint8_t* p = sec->data;
        GpuTexture& gt = textures[t];
        gt.w = rd_u32(p + 0);
        gt.h = rd_u32(p + 4);
        gt.tex = device.vram().alloc_buffer(gt.w, gt.h,
                                            gfx::PixelFormat::PSMT8, "dbg-tex");
        gt.clut = device.vram().alloc_buffer(16, 16, gfx::PixelFormat::PSMCT32,
                                             "dbg-clut");
        if (!gt.tex.valid() || !gt.clut.valid() ||
            !device.upload_texture(p + 16u + 1024u, gt.tex, gt.w, gt.h,
                                   gfx::PixelFormat::PSMT8) ||
            !device.upload_clut(reinterpret_cast<const uint32_t*>(p + 16u),
                                gt.clut, 256)) {
            printf("[22-scene-debug] texture %u (%ux%u) NOT resident\n",
                   static_cast<unsigned>(t), static_cast<unsigned>(gt.w),
                   static_cast<unsigned>(gt.h));
            device.vram().free(gt.tex);
            device.vram().free(gt.clut);
            gt = GpuTexture{};
        }
        device.end_frame();
    }

    vu::MicroProgram prog_unlit, prog_tex, prog_lit, prog_fog, prog_skin,
        prog_skin_tex;
    prog_unlit.set_blob(&VuUnlit_CodeStart, &VuUnlit_CodeEnd, 0);
    prog_tex.set_blob(&VuUnlitTex_CodeStart, &VuUnlitTex_CodeEnd, 300);
    prog_lit.set_blob(&VuLit_CodeStart, &VuLit_CodeEnd, 700);
    prog_fog.set_blob(&VuLitFog_CodeStart, &VuLitFog_CodeEnd, 1000);
    prog_skin.set_blob(&VuSkin_CodeStart, &VuSkin_CodeEnd, 1300);
    prog_skin_tex.set_blob(&VuSkinTex_CodeStart, &VuSkinTex_CodeEnd, 1500);
    if (!prog_unlit.upload() || !prog_tex.upload() || !prog_lit.upload() ||
        !prog_fog.upload() || !prog_skin.upload() || !prog_skin_tex.upload()) {
        printf("PS2UR_TOKEN_DBG_FAIL programs\n");
        SleepThread();
        return 1;
    }
    gfx::DmaChain chain;
    if (!chain.init(12288)) {
        printf("PS2UR_TOKEN_DBG_FAIL chain\n");
        SleepThread();
        return 1;
    }
    if (!world.has_camera()) {
        printf("PS2UR_TOKEN_DBG_FAIL no camera\n");
        SleepThread();
        return 1;
    }

    scene::SceneRenderer renderer;
    scene::RendererPrograms programs;
    BindContext bind_ctx{&device, textures, tex_count < 64u ? tex_count : 64u};

    renderer.debug_next_frame();
    // 4 seconds of playback: enough for any idle to visibly leave the bind
    // pose, so a T-posed character in the dumped frame means FROZEN, not
    // merely slow.
    for (uint32_t frame = 0; frame < 120; ++frame) {
        world.update_animators(1.0f / 30.0f);
        world.update_world_matrices();
        scene::RenderStats stats;
        if (!renderer.render(device, chain, world, programs, bind_texture,
                             &bind_ctx, &stats)) {
            printf("PS2UR_TOKEN_DBG_FAIL render frame %u\n",
                   static_cast<unsigned>(frame));
            SleepThread();
            return 1;
        }
        if (frame == 0 || frame == 119) {
            printf("[22-scene-debug] frame %u: drawn %u culled %u kicks %u "
                   "skinned %u\n",
                   static_cast<unsigned>(frame),
                   static_cast<unsigned>(stats.drawn),
                   static_cast<unsigned>(stats.culled),
                   static_cast<unsigned>(stats.kicks),
                   static_cast<unsigned>(stats.skinned_drawn));
        }
        if (world.skinned_renderer_count() > 0 && frame % 30 == 0) {
            const anim::Animator& a =
                world.animator(world.skinned_renderer(0).animator);
            const anim::Skeleton& sk = *a.skeleton();
            float dev = 0.0f;
            for (uint32_t b = 0; b < sk.bone_count; ++b) {
                const Quat r = a.pose().rot[b];
                const Quat rest = sk.bones[b].rest_rot;
                const float d = r.x * rest.x + r.y * rest.y + r.z * rest.z +
                                r.w * rest.w;
                const float e = 1.0f - (d < 0.0f ? -d : d);
                if (e > dev) dev = e;
            }
            printf("[dbg-anim] f%u state %u pose-vs-rest %f world13 %f\n",
                   static_cast<unsigned>(frame),
                   static_cast<unsigned>(a.state()), dev,
                   a.bone_world(5).m[13]);
        }
    }
    // The rendered frame itself, written back through the emulator's host
    // filesystem: diagnostics you can LOOK at, not just read about.
    if (device.read_framebuffer(g_frame, 0, 0, 512, 448)) {
        int fd = open("host:debugframe.bin", O_WRONLY | O_CREAT | O_TRUNC);
        if (fd >= 0) {
            write(fd, g_frame, sizeof(g_frame));
            close(fd);
            printf("[22-scene-debug] frame written to debugframe.bin "
                   "(512x448 RGBA)\n");
        }
    }

    printf("PS2UR_TOKEN_DBG_OK\n");
    SleepThread();
    return 0;
}
