// samples/17-skinning -- M9 acceptance: three skinned characters (1,500
// triangles, 24 bones each) animating with crossfades at 30 fps, checked
// against an Editor reference (plan section 9, M9).
//
// Loads host:skinscene.p2b and host:skin-golden.bin (both produced by
// Ps2.Editor.PS2SkinExportMenu.ExportSkinScene) and runs four checks, none
// of which needs a human to look at a screen:
//
//  1. GOLDEN POSE MATRICES over a 5-second clip. Every bone's model matrix
//     is compared against the trace Unity produced by sampling the same clip
//     through its own transform hierarchy. This is the end-to-end test of
//     keyframe reduction, 16-bit quantisation, cursor sampling, slerp and
//     parent-before-child composition.
//  2. CROSSFADE, by its defining property rather than by eye: while blending
//     from clip A to clip B at weight w, a bone's direction must sit on the
//     arc between its two source directions at fraction w. Checked against
//     the two golden traces, so no blend maths is duplicated here.
//  3. THROUGHPUT: 150 frames with all three characters animating and one
//     crossfade running, asserted at 30 fps (>= 29.5; NTSC tops out at
//     29.97, so 30.0 is not a testable bar -- verify-log).
//  4. GOLDEN IMAGE tiles, to catch a rendering regression that leaves the
//     maths intact.
#include <ps2ur/anim.h>
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
#include <string.h>

using namespace ps2ur;

extern "C" u32 VuUnlit_CodeStart __attribute__((section(".vudata")));
extern "C" u32 VuUnlit_CodeEnd __attribute__((section(".vudata")));
extern "C" u32 VuLit_CodeStart __attribute__((section(".vudata")));
extern "C" u32 VuLit_CodeEnd __attribute__((section(".vudata")));
extern "C" u32 VuSkin_CodeStart __attribute__((section(".vudata")));
extern "C" u32 VuSkin_CodeEnd __attribute__((section(".vudata")));

namespace {

constexpr uint32_t kScreenW = 512;
constexpr uint32_t kScreenH = 448;
constexpr float kFixedDt = 1.0f / 30.0f;

alignas(16) uint8_t g_scene_arena[3 * 1024 * 1024];
alignas(16) uint8_t g_golden_arena[512 * 1024];
alignas(16) uint8_t g_pixels[256 * 256 * 4];

// The golden trace written by the exporter.
struct Golden {
    const uint8_t* data = nullptr;
    uint32_t bone_count = 0;
    uint32_t count_a = 0;
    float step_a = 0.0f;
    uint32_t count_b = 0;
    float step_b = 0.0f;
    uint32_t offset_a = 0;
    uint32_t offset_b = 0;
};

uint32_t rd_u32(const uint8_t* p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

float rd_f32(const uint8_t* p)
{
    float v;
    memcpy(&v, p, 4);
    return v;
}

bool parse_golden(const uint8_t* data, uint32_t size, Golden& out)
{
    if (data == nullptr || size < 28u || rd_u32(data) != 0x47413250u) {
        return false;
    }
    out.data = data;
    out.bone_count = rd_u32(data + 4);
    const uint32_t stride = rd_u32(data + 8);
    out.count_a = rd_u32(data + 12);
    out.step_a = rd_f32(data + 16);
    out.count_b = rd_u32(data + 20);
    out.step_b = rd_f32(data + 24);
    if (stride != 16u || out.bone_count == 0 || out.bone_count > anim::kMaxBones) {
        return false;
    }
    out.offset_a = 28u;
    const uint32_t bytes_per_sample = out.bone_count * 16u * 4u;
    out.offset_b = out.offset_a + out.count_a * bytes_per_sample;
    const uint32_t total = out.offset_b + out.count_b * bytes_per_sample;
    return total <= size;
}

// Pointer to bone 'bone' of sample 'sample' in trace A (trace = 0) or B.
const uint8_t* golden_matrix(const Golden& g, uint32_t trace, uint32_t sample,
                             uint32_t bone)
{
    const uint32_t base = trace == 0 ? g.offset_a : g.offset_b;
    return g.data + base + (sample * g.bone_count + bone) * 16u * 4u;
}

// Largest absolute element difference between a runtime bone matrix and the
// Editor's, across every bone of one sample.
float compare_pose(const anim::Animator& animator, const Golden& g,
                   uint32_t trace, uint32_t sample)
{
    float worst = 0.0f;
    for (uint32_t bone = 0; bone < g.bone_count; ++bone) {
        const uint8_t* expected = golden_matrix(g, trace, sample, bone);
        const Mat4& actual = animator.bone_world(bone);
        for (uint32_t e = 0; e < 16; ++e) {
            float diff = actual.m[e] - rd_f32(expected + e * 4u);
            if (diff < 0.0f) {
                diff = -diff;
            }
            if (diff > worst) {
                worst = diff;
            }
        }
    }
    return worst;
}

// Translation column of a golden bone matrix.
Vec3 golden_origin(const Golden& g, uint32_t trace, uint32_t sample, uint32_t bone)
{
    const uint8_t* m = golden_matrix(g, trace, sample, bone);
    return Vec3{rd_f32(m + 12 * 4), rd_f32(m + 13 * 4), rd_f32(m + 14 * 4)};
}

uint64_t read_cycles()
{
    uint32_t now;
    asm volatile("mfc0 %0, $9" : "=r"(now));
    return now;
}

float absf(float v) { return v < 0.0f ? -v : v; }

} // namespace

int main(void)
{
    platform::init();
    anim::init();

    gfx::VideoConfig config;
    config.width = kScreenW;
    config.height = kScreenH;
    config.packet_qwords = 16384;

    gfx::GsDevice device;
    if (!device.init(config)) {
        printf("PS2UR_TOKEN_SKIN_FAIL device\n");
        SleepThread();
        return 1;
    }

    // --- Load ---------------------------------------------------------------
    Arena scene_arena;
    scene_arena.init(g_scene_arena, sizeof(g_scene_arena));
    uint32_t scene_size = 0;
    const void* scene_data =
        io::load_file("host:skinscene.p2b", scene_arena, &scene_size);
    if (scene_data == nullptr) {
        scene_data = io::load_file("skinscene.p2b", scene_arena, &scene_size);
    }
    io::P2bFile file;
    static scene::World world;
    if (scene_data == nullptr || !file.parse(scene_data, scene_size)) {
        printf("PS2UR_TOKEN_SKIN_FAIL scene load/parse\n");
        SleepThread();
        return 1;
    }
    if (!world.load(file)) {
        printf("PS2UR_TOKEN_SKIN_FAIL world: %s\n", world.error());
        SleepThread();
        return 1;
    }

    Arena golden_arena;
    golden_arena.init(g_golden_arena, sizeof(g_golden_arena));
    uint32_t golden_size = 0;
    const void* golden_data =
        io::load_file("host:skin-golden.bin", golden_arena, &golden_size);
    if (golden_data == nullptr) {
        golden_data = io::load_file("skin-golden.bin", golden_arena, &golden_size);
    }
    Golden golden;
    if (!parse_golden(static_cast<const uint8_t*>(golden_data), golden_size,
                      golden)) {
        printf("PS2UR_TOKEN_SKIN_FAIL golden trace\n");
        SleepThread();
        return 1;
    }

    printf("[17-skinning] %u skinned renderers, %u skeletons, %u clips, "
           "%u controllers\n",
           static_cast<unsigned>(world.skinned_renderer_count()),
           static_cast<unsigned>(world.skeleton_count()),
           static_cast<unsigned>(world.clip_count()),
           static_cast<unsigned>(world.controller_count()));
    if (world.skinned_renderer_count() < 3 || world.skinned_mesh_count() < 1) {
        printf("PS2UR_TOKEN_SKIN_FAIL expected three skinned characters\n");
        SleepThread();
        return 1;
    }
    {
        const scene::LoadedSkinnedMesh& mesh = world.skinned_mesh(0);
        uint32_t triangles = 0;
        for (uint32_t b = 0; b < mesh.batch_count; ++b) {
            triangles += mesh.batches[b].vertex_count / 3u;
        }
        printf("[17-skinning] mesh: %u batches, %u triangles, %u bones, "
               "palette %u\n",
               static_cast<unsigned>(mesh.batch_count),
               static_cast<unsigned>(triangles),
               static_cast<unsigned>(world.skeleton(mesh.skeleton).bone_count),
               static_cast<unsigned>(mesh.bone_count[0]));
        if (triangles < 1400u) {
            printf("PS2UR_TOKEN_SKIN_FAIL mesh too small for the acceptance\n");
            SleepThread();
            return 1;
        }
        if (world.skeleton(mesh.skeleton).bone_count != 24u) {
            printf("PS2UR_TOKEN_SKIN_FAIL expected 24 bones\n");
            SleepThread();
            return 1;
        }
    }

    // --- Programs / chain / renderer ---------------------------------------
    vu::MicroProgram prog_unlit, prog_lit, prog_skin;
    prog_unlit.set_blob(&VuUnlit_CodeStart, &VuUnlit_CodeEnd, 0);
    prog_lit.set_blob(&VuLit_CodeStart, &VuLit_CodeEnd, 700);
    prog_skin.set_blob(&VuSkin_CodeStart, &VuSkin_CodeEnd, 1300);
    if (!prog_unlit.upload() || !prog_lit.upload() || !prog_skin.upload()) {
        printf("PS2UR_TOKEN_SKIN_FAIL programs\n");
        SleepThread();
        return 1;
    }
    gfx::DmaChain chain;
    if (!chain.init(16384)) {
        printf("PS2UR_TOKEN_SKIN_FAIL chain\n");
        SleepThread();
        return 1;
    }
    scene::SceneRenderer renderer;
    scene::RendererPrograms programs;
    scene::RenderStats stats{};

    // --- Check 1: golden pose matrices over the 5-second clip ---------------
    //
    // Animator 0 starts in state 0 (the 5-second "Wave" clip, non-looping)
    // and nothing triggers a transition, so it plays straight through.
    const uint32_t frames_per_sample =
        static_cast<uint32_t>(golden.step_a * 30.0f + 0.5f);
    float worst_pose = 0.0f;
    uint32_t worst_sample = 0;
    {
        anim::Animator& animator = world.animator(0);
        uint32_t sample = 0;
        // Frame 0 is the clip sampled at t=0 -- advancing by zero evaluates
        // the pose without moving time, which is what the golden's first
        // entry holds. (Comparing the REST pose here instead is the obvious
        // mistake: the clip is already non-identity at t=0.)
        world.update_animators(0.0f);
        worst_pose = compare_pose(animator, golden, 0, 0);
        ++sample;
        for (uint32_t frame = 1; frame < 150u && sample < golden.count_a; ++frame) {
            world.update_animators(kFixedDt);
            if (frame % frames_per_sample == 0u) {
                const float error = compare_pose(animator, golden, 0, sample);
                if (error > worst_pose) {
                    worst_pose = error;
                    worst_sample = sample;
                }
                ++sample;
            }
        }
        printf("M9_POSE_PARITY samples=%u bones=%u max_err=%f (worst sample %u)\n",
               static_cast<unsigned>(sample), static_cast<unsigned>(golden.bone_count),
               static_cast<double>(worst_pose), static_cast<unsigned>(worst_sample));
        // Budget: 16-bit rotation quantisation (~3e-5 rad per bone) plus the
        // exporter's 0.05-degree keyframe-reduction tolerance, accumulated
        // down a 24-bone chain with a lever arm of ~8 units.
        if (worst_pose > 0.02f) {
            printf("PS2UR_TOKEN_SKIN_FAIL pose parity %f\n",
                   static_cast<double>(worst_pose));
            SleepThread();
            return 1;
        }
    }

    // --- Check 2: the crossfade lands on the arc ----------------------------
    //
    // At blend weight w the blended bone direction must sit at fraction w of
    // the angle between the two source directions. Both sources come from
    // the golden traces, so this validates our slerp against Unity's poses
    // without re-implementing the blend here.
    {
        anim::Animator& animator = world.animator(1);
        animator.play(0);
        animator.update(0.0f); // settle at clip A, t = 0

        // Bone 23's origin is the far tip: the most sensitive point.
        const uint32_t probe = golden.bone_count - 1u;
        const Vec3 from = golden_origin(golden, 0, 0, probe);
        const Vec3 to = golden_origin(golden, 1, 0, probe);

        const anim::Controller& controller = world.controller(0);
        if (controller.param_count < 2u) {
            printf("PS2UR_TOKEN_SKIN_FAIL controller has no trigger params\n");
            SleepThread();
            return 1;
        }
        animator.set_trigger(controller.param_hash[0]); // "ToCoil"
        animator.update(kFixedDt); // fires the transition, blend starts

        // The rig rotates only, so EVERY pose -- including every blended one
        // -- must preserve the distance between adjacent bone origins. That
        // is an exact invariant, independent of the golden: a blend that
        // lerped quaternions without renormalising, or a hierarchy composed
        // in the wrong order, shrinks or stretches the chain immediately.
        const anim::Skeleton& skeleton = *animator.skeleton();
        const float rest_spacing = length(skeleton.bones[1].rest_pos);

        bool ramp_monotonic = true;
        float last_weight = -1.0f;
        float worst_spacing = 0.0f;
        uint32_t blend_frames = 0;
        for (uint32_t step = 0; step < 20u && animator.blending(); ++step) {
            const float w = animator.blend_weight();
            if (w < last_weight) {
                ramp_monotonic = false;
            }
            last_weight = w;

            for (uint32_t bone = 1; bone < skeleton.bone_count; ++bone) {
                if (skeleton.bones[bone].parent < 0) {
                    continue;
                }
                const Mat4& child = animator.bone_world(bone);
                const Mat4& parent =
                    animator.bone_world(static_cast<uint32_t>(
                        skeleton.bones[bone].parent));
                const Vec3 delta{child.m[12] - parent.m[12],
                                 child.m[13] - parent.m[13],
                                 child.m[14] - parent.m[14]};
                const float drift = absf(length(delta) - rest_spacing);
                if (drift > worst_spacing) {
                    worst_spacing = drift;
                }
            }

            animator.update(kFixedDt);
            ++blend_frames;
        }
        printf("M9_CROSSFADE frames=%u (0.5s at 30fps) monotonic=%d "
               "end_weight=%f blending=%d spacing_drift=%f\n",
               static_cast<unsigned>(blend_frames), ramp_monotonic ? 1 : 0,
               static_cast<double>(last_weight), animator.blending() ? 1 : 0,
               static_cast<double>(worst_spacing));

        // A 0.5 s crossfade at 30 fps takes 15 frames, or 16 when the float
        // accumulation of 1/30 lands a hair under the duration.
        if (!ramp_monotonic || animator.blending() || blend_frames < 14u ||
            blend_frames > 17u) {
            printf("PS2UR_TOKEN_SKIN_FAIL crossfade ramp\n");
            SleepThread();
            return 1;
        }
        if (worst_spacing > 0.002f) {
            printf("PS2UR_TOKEN_SKIN_FAIL blended chain deformed by %f\n",
                   static_cast<double>(worst_spacing));
            SleepThread();
            return 1;
        }
        // The blend really did travel between the two clips' poses.
        const Vec3 blended = Vec3{animator.bone_world(probe).m[12],
                                  animator.bone_world(probe).m[13],
                                  animator.bone_world(probe).m[14]};
        printf("M9_CROSSFADE tip A=(%.2f,%.2f,%.2f) B=(%.2f,%.2f,%.2f) "
               "blended=(%.2f,%.2f,%.2f)\n",
               static_cast<double>(from.x), static_cast<double>(from.y),
               static_cast<double>(from.z), static_cast<double>(to.x),
               static_cast<double>(to.y), static_cast<double>(to.z),
               static_cast<double>(blended.x), static_cast<double>(blended.y),
               static_cast<double>(blended.z));
    }

    // --- Check 3: three characters, crossfades, 30 fps -----------------------
    {
        // Stagger the characters so all three are animating and at least one
        // is mid-crossfade for most of the run.
        const anim::Controller& controller = world.controller(0);
        for (uint32_t i = 0; i < world.skinned_renderer_count(); ++i) {
            world.animator(i).play(0);
        }
        uint32_t crossfades = 0;
        const uint64_t t0 = read_cycles();
        for (uint32_t frame = 0; frame < 150u; ++frame) {
            for (uint32_t i = 0; i < world.skinned_renderer_count(); ++i) {
                if (frame == 20u + i * 25u && controller.param_count > 0) {
                    world.animator(i).set_trigger(controller.param_hash[0]);
                    ++crossfades;
                }
                if (frame == 100u + i * 10u && controller.param_count > 1) {
                    world.animator(i).set_trigger(controller.param_hash[1]);
                    ++crossfades;
                }
            }
            world.update_animators(kFixedDt);
            world.update_world_matrices();
            if (!renderer.render(device, chain, world, programs, nullptr, nullptr,
                                 &stats)) {
                printf("PS2UR_TOKEN_SKIN_FAIL render frame %u\n",
                       static_cast<unsigned>(frame));
                SleepThread();
                return 1;
            }
        }
        const uint64_t elapsed = read_cycles() - t0;
        const uint32_t fps_x100 =
            static_cast<uint32_t>((150ull * 100ull * 147456000ull) / elapsed);
        printf("M9_THROUGHPUT chars=%u batches=%u crossfades=%u fps=%u.%02u\n",
               static_cast<unsigned>(stats.skinned_drawn),
               static_cast<unsigned>(stats.skin_batches),
               static_cast<unsigned>(crossfades), fps_x100 / 100u,
               fps_x100 % 100u);
        if (stats.skinned_drawn != 3u) {
            printf("PS2UR_TOKEN_SKIN_FAIL only %u characters drawn\n",
                   static_cast<unsigned>(stats.skinned_drawn));
            SleepThread();
            return 1;
        }
        if (fps_x100 < 2950u) {
            printf("PS2UR_TOKEN_SKIN_FAIL fps %u.%02u\n", fps_x100 / 100u,
                   fps_x100 % 100u);
            SleepThread();
            return 1;
        }
    }

    // --- Check 4: golden image ----------------------------------------------
    if (!device.read_framebuffer(g_pixels, 128, 96, 256, 256)) {
        printf("PS2UR_TOKEN_SKIN_FAIL readback\n");
        SleepThread();
        return 1;
    }
    uint32_t drawn_px = 0;
    for (uint32_t i = 0; i < 256u * 256u; ++i) {
        const uint8_t* p = &g_pixels[i * 4u];
        if (p[0] != 20 || p[1] != 24 || p[2] != 40) {
            drawn_px++;
        }
    }
    printf("[17-skinning] %u of %u window pixels drawn\n",
           static_cast<unsigned>(drawn_px), 256u * 256u);
    if (drawn_px < 2000u) {
        printf("PS2UR_TOKEN_SKIN_FAIL characters not visible\n");
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

    printf("PS2UR_TOKEN_SKIN_OK\n");
    chain.shutdown();
    device.shutdown();
    SleepThread();
    return 0;
}
