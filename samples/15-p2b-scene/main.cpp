// samples/15-p2b-scene -- M5 acceptance: a scene authored in Unity renders on
// target with correct transforms.
//
// Loads host:m5-scene.p2b (exported by Ps2.Editor.PS2ExportMenu.ExportTestScene
// and staged next to the ELF by run-emu-test.sh), uploads its textures, and
// renders every entity through the VU1 chain path: unlit, textured and lit
// materials each through their own microprogram, resident simultaneously at
// micro addresses 0 / 300 / 700.
//
// Verification without eyes: prints entity/mesh/texture counts, per-frame
// chain stats, then golden tile CRCs over a centre window. The golden is
// captured once (goldens/check.sh --update) and diffed thereafter.
//
// Recorded deviation from the plan's M5 acceptance: the comparison reference
// is a checked-in golden of THIS renderer, not an Editor-side render through
// an equivalent constrained path -- that Editor renderer does not exist yet.
// Determinism + goldens still catch every regression on the PS2 side.

#include <ps2ur/dma_chain.h>
#include <ps2ur/gs_batch.h>
#include <ps2ur/gs_device.h>
#include <ps2ur/log.h>
#include <ps2ur/math.h>
#include <ps2ur/p2b.h>
#include <ps2ur/p2b_scene.h>
#include <ps2ur/platform.h>
#include <ps2ur/vu_program.h>

#include <kernel.h>
#include <math.h>
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

// Micro-memory placement: all three programs resident at once.
constexpr uint32_t kAddrUnlit = 0;
constexpr uint32_t kAddrTex = 300;
constexpr uint32_t kAddrLit = 700;

alignas(16) uint8_t g_file_arena_mem[3 * 1024 * 1024];
alignas(16) gfx::Qword g_constants[17];
alignas(16) uint8_t g_pixels[256 * 256 * 4];

struct GpuTexture {
    gfx::VramAlloc tex;
    gfx::VramAlloc clut;
    uint32_t w = 0, h = 0;
    uint32_t fmt = 0;
};

void set_float4(gfx::Qword& q, float x, float y, float z, float w)
{
    union {
        float f;
        uint32_t u;
    } cx{x}, cy{y}, cz{z}, cw{w};
    q.lo = static_cast<uint64_t>(cx.u) | (static_cast<uint64_t>(cy.u) << 32);
    q.hi = static_cast<uint64_t>(cz.u) | (static_cast<uint64_t>(cw.u) << 32);
}

uint32_t rd_u32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

int main(void)
{
    platform::init();

    gfx::VideoConfig config;
    config.width = kScreenW;
    config.height = kScreenH;
    config.packet_qwords = 16384; // texture uploads at load time

    gfx::GsDevice device;
    if (!device.init(config)) {
        printf("PS2UR_TOKEN_P2B_FAIL device\n");
        SleepThread();
        return 1;
    }

    // --- Load the container -------------------------------------------------
    Arena file_arena;
    file_arena.init(g_file_arena_mem, sizeof(g_file_arena_mem));
    uint32_t file_size = 0;
    // The host filesystem device name varies by loader (PCSX2 HLE, ps2link):
    // probe the known spellings and remember which one worked.
    const char* candidates[] = {"host:m5-scene.p2b", "host0:m5-scene.p2b",
                                "host:./m5-scene.p2b", "m5-scene.p2b"};
    const void* file_data = nullptr;
    for (uint32_t ci = 0; ci < 4 && file_data == nullptr; ++ci) {
        printf("[15-p2b-scene] trying '%s'\n", candidates[ci]);
        file_data = io::load_file(candidates[ci], file_arena, &file_size);
    }
    if (file_data == nullptr) {
        printf("PS2UR_TOKEN_P2B_FAIL load\n");
        SleepThread();
        return 1;
    }
    io::P2bFile file;
    if (!file.parse(file_data, file_size)) {
        printf("PS2UR_TOKEN_P2B_FAIL parse: %s\n", file.error());
        SleepThread();
        return 1;
    }
    static scene::World world;
    if (!world.load(file)) {
        printf("PS2UR_TOKEN_P2B_FAIL world: %s\n", world.error());
        SleepThread();
        return 1;
    }
    printf("[15-p2b-scene] %u bytes: %u entities, %u meshes, %u materials, %u textures\n",
           static_cast<unsigned>(file_size), static_cast<unsigned>(world.entity_count()),
           static_cast<unsigned>(world.mesh_count()),
           static_cast<unsigned>(world.material_count()),
           static_cast<unsigned>(file.count_of(io::kSectionTex)));

    // --- Upload textures ----------------------------------------------------
    GpuTexture textures[16];
    const uint32_t tex_count = file.count_of(io::kSectionTex);
    if (tex_count > 16) {
        printf("PS2UR_TOKEN_P2B_FAIL too many textures\n");
        SleepThread();
        return 1;
    }
    device.begin_frame();
    device.clear(0, 0, 0);
    for (uint32_t t = 0; t < tex_count; ++t) {
        const io::P2bSection* sec = file.find(io::kSectionTex, t);
        const uint8_t* p = sec->data;
        GpuTexture& gt = textures[t];
        gt.w = rd_u32(p + 0);
        gt.h = rd_u32(p + 4);
        gt.fmt = rd_u32(p + 8);
        const uint32_t clut_entries = rd_u32(p + 12);
        if (gt.fmt != 0x13u || clut_entries != 256u ||
            sec->size < 16u + 1024u + gt.w * gt.h) {
            printf("PS2UR_TOKEN_P2B_FAIL texture %u malformed\n",
                   static_cast<unsigned>(t));
            SleepThread();
            return 1;
        }
        gt.tex = device.vram().alloc_buffer(gt.w, gt.h, gfx::PixelFormat::PSMT8,
                                            "scene-tex");
        gt.clut = device.vram().alloc_clut("scene-clut");
        if (!gt.tex.valid() || !gt.clut.valid() ||
            !device.upload_texture(p + 16u + 1024u, gt.tex, gt.w, gt.h,
                                   gfx::PixelFormat::PSMT8) ||
            !device.upload_clut(reinterpret_cast<const uint32_t*>(p + 16u), gt.clut,
                                256)) {
            printf("PS2UR_TOKEN_P2B_FAIL texture %u upload\n",
                   static_cast<unsigned>(t));
            SleepThread();
            return 1;
        }
    }
    device.end_frame();

    // --- Programs -----------------------------------------------------------
    vu::MicroProgram prog_unlit, prog_tex, prog_lit;
    prog_unlit.set_blob(&VuUnlit_CodeStart, &VuUnlit_CodeEnd, kAddrUnlit);
    prog_tex.set_blob(&VuUnlitTex_CodeStart, &VuUnlitTex_CodeEnd, kAddrTex);
    prog_lit.set_blob(&VuLit_CodeStart, &VuLit_CodeEnd, kAddrLit);
    if (!prog_unlit.upload() || !prog_tex.upload() || !prog_lit.upload()) {
        printf("PS2UR_TOKEN_P2B_FAIL programs\n");
        SleepThread();
        return 1;
    }

    gfx::DmaChain chain;
    if (!chain.init(2048)) {
        printf("PS2UR_TOKEN_P2B_FAIL chain\n");
        SleepThread();
        return 1;
    }

    // --- Camera -------------------------------------------------------------
    if (!world.has_camera()) {
        printf("PS2UR_TOKEN_P2B_FAIL no camera\n");
        SleepThread();
        return 1;
    }
    const scene::Camera& cam = world.camera();
    const Mat4 proj = mat4_perspective(cam.fov, 512.0f / 448.0f, cam.znear, cam.zfar);
    // Unity scenes are left-handed with the camera looking down +z; the
    // projection is right-handed. flipz converts between them; winding
    // mirrors, which is harmless with no backface culling on the GS.
    Mat4 flipz = mat4_identity();
    flipz.m[10] = -1.0f;

    const float sx = static_cast<float>(kScreenW) * 0.5f;
    const float sy = -static_cast<float>(kScreenH) * 0.5f;
    const float zmax = 8388607.0f;
    const float szf = -zmax * 0.5f / 16.0f;
    const float ozf = zmax * 0.5f / 16.0f;
    float vscale[3] = {sx, sy, szf};
    float voffset[3] = {sx + 2048.0f, -sy + 2048.0f, ozf};

    // --- Frame loop ---------------------------------------------------------
    const uint32_t kFrames = 8; // static scene: determinism is the point
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        world.update_world_matrices();
        const Mat4 view = mat4_rigid_inverse(world.world_matrix(
            static_cast<uint32_t>(world.camera().entity)));
        const Mat4 viewproj = mat4_mul(proj, mat4_mul(flipz, view));

        device.begin_frame();
        device.clear(24, 28, 44);
        device.end_frame();

        // Pass 1: untextured entities (unlit + lit) in one chain.
        chain.begin();
        bool ok = true;
        for (uint32_t e = 0; e < world.entity_count() && ok; ++e) {
            const scene::Entity& ent = world.entity(e);
            if (ent.mesh < 0) {
                continue;
            }
            const scene::LoadedMesh& mesh = world.mesh(static_cast<uint32_t>(ent.mesh));
            const scene::LoadedMaterial& mat = world.material(mesh.material_index);
            if (mat.kind == scene::kMaterialUnlitTextured) {
                continue;
            }
            const Mat4 mvp = mat4_mul(viewproj, world.world_matrix(e));

            if (mat.kind == scene::kMaterialVertexLit && world.has_light()) {
                // Light into object space: transpose of the world rotation.
                // Assumes uniform scale (recorded v1 limitation).
                const Mat4& w = world.world_matrix(e);
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
                // 12..14 are the colours of lights 0..2 as (r,g,b,0), not a
                // per-channel list (verify-log M9); light factor ~0..1
                // against 0..255 vertex colours (verify-log M8).
                set_float4(g_constants[12], lc.x, lc.y, lc.z, 0);
                set_float4(g_constants[13], 0, 0, 0, 0);
                set_float4(g_constants[14], 0, 0, 0, 0);
                set_float4(g_constants[15], 0.157f, 0.157f, 0.157f, 0);
                set_float4(g_constants[16], 255.0f, 255.0f, 255.0f, 128.0f);
                ok = chain.add_constants(g_constants, 17, 0);
            } else {
                gfx::BatchBuilder::build_unlit_constants(mvp.m, vscale, voffset,
                                                         4095.0f, cam.znear,
                                                         g_constants);
                ok = chain.add_constants(g_constants, 7, 0);
            }
            const uint32_t addr =
                mat.kind == scene::kMaterialVertexLit ? kAddrLit : kAddrUnlit;
            for (uint32_t b = 0; b < mesh.batch_count && ok; ++b) {
                ok = chain.add_batch(mesh.batches[b], addr);
            }
        }
        if (!ok || !chain.kick()) {
            printf("PS2UR_TOKEN_P2B_FAIL chain pass1 frame %u\n",
                   static_cast<unsigned>(frame));
            SleepThread();
            return 1;
        }
        chain.wait();

        // Pass 2: textured entities, grouped by texture so TEX0 (PATH3) is
        // never racing PATH1 drawing.
        for (uint32_t t = 0; t < tex_count; ++t) {
            device.packet().reset();
            device.set_texture_indexed(textures[t].tex, textures[t].w,
                                       textures[t].h, gfx::PixelFormat::PSMT8,
                                       textures[t].clut, 256);
            device.flush_packet();

            chain.begin();
            bool any = false;
            ok = true;
            for (uint32_t e = 0; e < world.entity_count() && ok; ++e) {
                const scene::Entity& ent = world.entity(e);
                if (ent.mesh < 0) {
                    continue;
                }
                const scene::LoadedMesh& mesh =
                    world.mesh(static_cast<uint32_t>(ent.mesh));
                const scene::LoadedMaterial& mat =
                    world.material(mesh.material_index);
                if (mat.kind != scene::kMaterialUnlitTextured ||
                    mat.texture_index != t) {
                    continue;
                }
                const Mat4 mvp = mat4_mul(viewproj, world.world_matrix(e));
                gfx::BatchBuilder::build_unlit_constants(mvp.m, vscale, voffset,
                                                         4095.0f, cam.znear,
                                                         g_constants);
                ok = chain.add_constants(g_constants, 7, 0);
                for (uint32_t b = 0; b < mesh.batch_count && ok; ++b) {
                    ok = chain.add_batch(mesh.batches[b], kAddrTex);
                }
                any = true;
            }
            if (!ok) {
                printf("PS2UR_TOKEN_P2B_FAIL chain tex %u\n",
                       static_cast<unsigned>(t));
                SleepThread();
                return 1;
            }
            if (any) {
                if (!chain.kick()) {
                    printf("PS2UR_TOKEN_P2B_FAIL kick tex %u\n",
                           static_cast<unsigned>(t));
                    SleepThread();
                    return 1;
                }
                chain.wait();
            }
        }
    }

    const gfx::DmaChain::Stats& st = chain.stats();
    printf("[15-p2b-scene] last chain: %u batches, %u qwords\n",
           static_cast<unsigned>(st.batches), static_cast<unsigned>(st.qwords));

    // --- Golden -------------------------------------------------------------
    if (!device.read_framebuffer(g_pixels, 128, 96, 256, 256)) {
        printf("PS2UR_TOKEN_P2B_FAIL readback\n");
        SleepThread();
        return 1;
    }
    uint32_t lit_px = 0;
    for (uint32_t i = 0; i < 256u * 256u; ++i) {
        const uint8_t* p = &g_pixels[i * 4u];
        if (p[0] != 24 || p[1] != 28 || p[2] != 44) {
            lit_px++;
        }
    }
    printf("[15-p2b-scene] %u of %u window pixels drawn\n",
           static_cast<unsigned>(lit_px), 256u * 256u);
    if (lit_px < 2000) {
        printf("PS2UR_TOKEN_P2B_FAIL scene not visible\n");
        SleepThread();
        return 1;
    }

    printf("GOLDEN 256x256 at (128,96) tiles 8x8\n");
    for (uint32_t ty = 0; ty < 8; ++ty) {
        for (uint32_t tx = 0; tx < 8; ++tx) {
            printf("GOLDEN_TILE %u %u %08X\n", static_cast<unsigned>(tx),
                   static_cast<unsigned>(ty),
                   static_cast<unsigned>(
                       gfx::GsDevice::tile_crc32(g_pixels, 256, 256, tx, ty)));
        }
    }

    printf("PS2UR_TOKEN_P2B_OK\n");
    chain.shutdown();
    device.shutdown();
    SleepThread();
    return 0;
}
