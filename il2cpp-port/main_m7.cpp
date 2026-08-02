// M7 host program: the managed facade drives a native-rendered scene
// (plan section 9, M7 acceptance).
//
// Boot: il2cpp runtime up -> load spin-scene.p2b into scene::World ->
// bridge::bind_world -> instantiate script components through the managed
// Runtime -> 330 frames of { Runtime.Tick(1/30) ; world matrix pass ;
// VU1 render }. The managed SpinParityCheck prints the parity verdict
// against the Editor golden at frame 300; this file owns runtime bring-up,
// rendering, the dispatch-cost measurement (M7 task 4), and the final
// PS2UR_TOKEN_M7_* verdict.
#include <ps2ur/bridge.h>
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
#include <stdio.h>

#include "il2cpp-api.h"
#include "il2cpp-class-internals.h"

using namespace ps2ur;

extern "C" u32 VuUnlit_CodeStart __attribute__((section(".vudata")));
extern "C" u32 VuUnlit_CodeEnd __attribute__((section(".vudata")));
extern "C" u32 VuLit_CodeStart __attribute__((section(".vudata")));
extern "C" u32 VuLit_CodeEnd __attribute__((section(".vudata")));

// bdwgc stack bound (see il2cpp-port/os/ps2/GcSupport.cpp).
extern "C" char* ps2ur_gc_stack_bottom;

namespace {

constexpr uint32_t kScreenW = 512;
constexpr uint32_t kScreenH = 448;
constexpr uint32_t kAddrUnlit = 0;
constexpr uint32_t kAddrLit = 700;

alignas(16) uint8_t g_file_arena_mem[2 * 1024 * 1024];
alignas(16) gfx::Qword g_constants[17];
alignas(16) uint8_t g_pixels[256 * 256 * 4];

void set_float4(gfx::Qword& q, float x, float y, float z, float w)
{
    union {
        float f;
        uint32_t u;
    } cx{x}, cy{y}, cz{z}, cw{w};
    q.lo = static_cast<uint64_t>(cx.u) | (static_cast<uint64_t>(cy.u) << 32);
    q.hi = static_cast<uint64_t>(cz.u) | (static_cast<uint64_t>(cw.u) << 32);
}

uint64_t read_cop0_cycles()
{
    uint32_t now;
    asm volatile("mfc0 %0, $9" : "=r"(now));
    return now;
}

const MethodInfo* find_runtime_method(const char* name, int params)
{
    const Il2CppDomain* domain = il2cpp_domain_get();
    size_t count = 0;
    const Il2CppAssembly** assemblies = il2cpp_domain_get_assemblies(domain, &count);
    for (size_t i = 0; i < count; ++i) {
        const Il2CppImage* image = il2cpp_assembly_get_image(assemblies[i]);
        Il2CppClass* klass =
            il2cpp_class_from_name(image, "UnityEngine.Internal", "Runtime");
        if (klass != nullptr) {
            const MethodInfo* method =
                il2cpp_class_get_method_from_name(klass, name, params);
            if (method != nullptr) {
                return method;
            }
        }
    }
    return nullptr;
}

bool invoke_checked(const MethodInfo* method, void** args, const char* what)
{
    Il2CppException* exception = nullptr;
    il2cpp_runtime_invoke(const_cast<MethodInfo*>(method), nullptr, args, &exception);
    if (exception != nullptr) {
        static char message[512];
        il2cpp_format_exception(exception, message, sizeof(message));
        printf("[m7] %s threw: %s\n", what, message);
        return false;
    }
    return true;
}

} // namespace

int main(void)
{
    int stackAnchor;
    ps2ur_gc_stack_bottom = reinterpret_cast<char*>(
        (reinterpret_cast<unsigned long>(&stackAnchor) + 128) & ~15UL);

    platform::init();
    printf("[m7] managed facade bring-up\n");

    // --- Native scene -------------------------------------------------------
    gfx::VideoConfig config;
    config.width = kScreenW;
    config.height = kScreenH;

    gfx::GsDevice device;
    if (!device.init(config)) {
        printf("PS2UR_TOKEN_M7_FAIL device\n");
        SleepThread();
        return 1;
    }

    Arena file_arena;
    file_arena.init(g_file_arena_mem, sizeof(g_file_arena_mem));
    uint32_t file_size = 0;
    const void* file_data =
        io::load_file("host:spin-scene.p2b", file_arena, &file_size);
    if (file_data == nullptr) {
        file_data = io::load_file("spin-scene.p2b", file_arena, &file_size);
    }
    if (file_data == nullptr) {
        printf("PS2UR_TOKEN_M7_FAIL load\n");
        SleepThread();
        return 1;
    }
    io::P2bFile file;
    if (!file.parse(file_data, file_size)) {
        printf("PS2UR_TOKEN_M7_FAIL parse: %s\n", file.error());
        SleepThread();
        return 1;
    }
    scene::World world;
    if (!world.load(file)) {
        printf("PS2UR_TOKEN_M7_FAIL world: %s\n", world.error());
        SleepThread();
        return 1;
    }
    printf("[m7] scene: %u entities, %u meshes, %u scripts\n",
           static_cast<unsigned>(world.entity_count()),
           static_cast<unsigned>(world.mesh_count()),
           static_cast<unsigned>(world.script_count()));

    if (!bridge::init()) {
        printf("PS2UR_TOKEN_M7_FAIL bridge\n");
        SleepThread();
        return 1;
    }
    bridge::bind_world(&world);

    // --- Managed runtime ----------------------------------------------------
    il2cpp_set_data_dir("host:");
    if (!il2cpp_init("IL2CPP Root Domain")) {
        printf("PS2UR_TOKEN_M7_FAIL il2cpp_init\n");
        SleepThread();
        return 1;
    }
    const MethodInfo* create_script = find_runtime_method("CreateScript", 2);
    const MethodInfo* tick = find_runtime_method("Tick", 1);
    const MethodInfo* noop = find_runtime_method("Noop", 0);
    if (create_script == nullptr || tick == nullptr || noop == nullptr) {
        printf("PS2UR_TOKEN_M7_FAIL Runtime methods missing\n");
        SleepThread();
        return 1;
    }

    // --- Dispatch cost, both mechanisms (M7 task 4) -------------------------
    // (a) il2cpp_runtime_invoke per call; (b) the cached methodPointer as a
    // direct C call -- il2cpp's static-method ABI is (args..., MethodInfo*).
    {
        const int kCalls = 10000;
        uint64_t t0 = read_cop0_cycles();
        for (int i = 0; i < kCalls; ++i) {
            Il2CppException* ex = nullptr;
            il2cpp_runtime_invoke(const_cast<MethodInfo*>(noop), nullptr, nullptr, &ex);
        }
        uint64_t invoke_cycles = read_cop0_cycles() - t0;

        typedef void (*NoopFn)(const MethodInfo*);
        NoopFn direct = reinterpret_cast<NoopFn>(noop->methodPointer);
        t0 = read_cop0_cycles();
        for (int i = 0; i < kCalls; ++i) {
            direct(noop);
        }
        uint64_t direct_cycles = read_cop0_cycles() - t0;

        // 147.456 cycles per microsecond.
        printf("M7_DISPATCH runtime_invoke=%u us direct=%u us per 10k calls\n",
               static_cast<unsigned>(invoke_cycles / 147u),
               static_cast<unsigned>(direct_cycles / 147u));
    }

    // --- Instantiate scripts ------------------------------------------------
    for (uint32_t s = 0; s < world.script_count(); ++s) {
        const scene::ScriptRef& script = world.script(s);
        Il2CppString* type_name = il2cpp_string_new(script.type_name);
        int32_t handle = world.handle_of(script.entity);
        void* args[2] = {type_name, &handle};
        printf("[m7] script '%s' on entity %d\n", script.type_name, script.entity);
        if (!invoke_checked(create_script, args, "CreateScript")) {
            printf("PS2UR_TOKEN_M7_FAIL CreateScript\n");
            SleepThread();
            return 1;
        }
    }

    // --- Programs + chain ---------------------------------------------------
    vu::MicroProgram prog_unlit, prog_lit;
    prog_unlit.set_blob(&VuUnlit_CodeStart, &VuUnlit_CodeEnd, kAddrUnlit);
    prog_lit.set_blob(&VuLit_CodeStart, &VuLit_CodeEnd, kAddrLit);
    if (!prog_unlit.upload() || !prog_lit.upload()) {
        printf("PS2UR_TOKEN_M7_FAIL programs\n");
        SleepThread();
        return 1;
    }
    gfx::DmaChain chain;
    if (!chain.init(2048)) {
        printf("PS2UR_TOKEN_M7_FAIL chain\n");
        SleepThread();
        return 1;
    }

    if (!world.has_camera()) {
        printf("PS2UR_TOKEN_M7_FAIL no camera\n");
        SleepThread();
        return 1;
    }
    const scene::Camera& cam = world.camera();
    const Mat4 proj = mat4_perspective(cam.fov, 512.0f / 448.0f, cam.znear, cam.zfar);
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
    // 330 frames at a fixed dt of 1/30: the parity check completes at 300;
    // the tail proves the loop keeps running after the verdict. Two window
    // CRCs prove the RENDERED cube moved (the golden only proves the math).
    const float kFixedDt = 1.0f / 30.0f;
    uint32_t crc_frame_100 = 0;
    uint32_t crc_frame_260 = 0; // 100 vs 260: 160 frames * 3 deg = 480 deg, NOT a multiple of the cube symmetry (90 deg)

    for (uint32_t frame = 0; frame < 330; ++frame) {
        float dt = kFixedDt;
        void* tick_args[1] = {&dt};
        if (!invoke_checked(tick, tick_args, "Tick")) {
            printf("PS2UR_TOKEN_M7_FAIL Tick frame %u\n",
                   static_cast<unsigned>(frame));
            SleepThread();
            return 1;
        }

        world.update_world_matrices();
        const Mat4 view = mat4_rigid_inverse(world.world_matrix(
            static_cast<uint32_t>(world.camera().entity)));
        const Mat4 viewproj = mat4_mul(proj, mat4_mul(flipz, view));

        device.begin_frame();
        device.clear(24, 28, 44);
        device.end_frame();

        chain.begin();
        bool ok = true;
        for (uint32_t e = 0; e < world.entity_count() && ok; ++e) {
            const scene::Entity& ent = world.entity(e);
            if (ent.mesh < 0 || !ent.alive ||
                !world.entity_visible(static_cast<int32_t>(e))) {
                continue;
            }
            const scene::LoadedMesh& mesh = world.mesh(static_cast<uint32_t>(ent.mesh));
            const scene::LoadedMaterial& mat = world.material(mesh.material_index);
            if (mat.kind == scene::kMaterialUnlitTextured) {
                continue; // no textures in the spin scene
            }
            const Mat4 mvp = mat4_mul(viewproj, world.world_matrix(e));

            if (mat.kind == scene::kMaterialVertexLit && world.has_light()) {
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
                // Scale discipline (fixed at M8, verify-log): vertex colours
                // are 0..255, the light factor must be ~0..1.
                set_float4(g_constants[12], lc.x, 0, 0, 0);
                set_float4(g_constants[13], lc.y, 0, 0, 0);
                set_float4(g_constants[14], lc.z, 0, 0, 0);
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
            printf("PS2UR_TOKEN_M7_FAIL chain frame %u\n",
                   static_cast<unsigned>(frame));
            SleepThread();
            return 1;
        }
        chain.wait();

        if (frame == 100 || frame == 260) {
            if (!device.read_framebuffer(g_pixels, 128, 96, 256, 256)) {
                printf("PS2UR_TOKEN_M7_FAIL readback\n");
                SleepThread();
                return 1;
            }
            uint32_t crc = 0;
            for (uint32_t ty = 0; ty < 8; ++ty) {
                for (uint32_t tx = 0; tx < 8; ++tx) {
                    crc ^= gfx::GsDevice::tile_crc32(g_pixels, 256, 256, tx, ty);
                }
            }
            if (frame == 100) {
                crc_frame_100 = crc;
                uint32_t lit_px = 0;
                for (uint32_t i = 0; i < 256u * 256u; ++i) {
                    const uint8_t* p = &g_pixels[i * 4u];
                    if (p[0] != 24 || p[1] != 28 || p[2] != 44) {
                        lit_px++;
                    }
                }
                printf("[m7] frame 100: %u window pixels drawn\n",
                       static_cast<unsigned>(lit_px));
                if (lit_px < 500) {
                    printf("PS2UR_TOKEN_M7_FAIL cube not visible\n");
                    SleepThread();
                    return 1;
                }
            } else {
                crc_frame_260 = crc;
            }
        }
    }

    if (crc_frame_100 == crc_frame_260) {
        printf("PS2UR_TOKEN_M7_FAIL rendered frames identical (not spinning)\n");
        SleepThread();
        return 1;
    }
    printf("[m7] window CRCs differ across frames: %08X vs %08X\n",
           static_cast<unsigned>(crc_frame_100),
           static_cast<unsigned>(crc_frame_260));

    printf("PS2UR_TOKEN_M7_OK\n");
    chain.shutdown();
    il2cpp_shutdown();
    device.shutdown();
    SleepThread();
    return 0;
}
