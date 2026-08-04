// A rig with real FBX scale factors (M12.5): armature x12.25 folded into
// the rest chain, x100 mesh nodes, a 3.76 prefab root. M9's procedural
// rigs were scale-1 everywhere, so the runtime's scale path through pose
// composition was proven only by inspection until a real import used it.
//
// The expectations are MEASURED, not derived: the exporter's rig-diagnosis
// dump records Unity's own animator-relative matrices at export
// (<scene>.p2b.rigdiag.json), and this asserts the runtime composes the
// same numbers from the container. Skips when the scene is not staged;
// stage with:
//   cp <project>/Library/PS2Build/content/SampleScene.p2b build/
#include "ps2ur/p2b_scene.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <string>

using namespace ps2ur;
using namespace ps2ur::scene;

namespace {

bool file_exists(const char* path)
{
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        return false;
    }
    std::fclose(f);
    return true;
}

std::string find_scene(const char* name)
{
    const std::string roots[] = {
        "build/",          "../build/",          "../../build/",
        "../../../build/", "../../../../build/", "../../../../../build/",
    };
    for (const std::string& root : roots) {
        const std::string candidate = root + name;
        if (file_exists(candidate.c_str())) {
            return candidate;
        }
    }
    return std::string();
}

float column_scale(const Mat4& m, int c)
{
    return std::sqrt(m.m[c * 4 + 0] * m.m[c * 4 + 0] +
                     m.m[c * 4 + 1] * m.m[c * 4 + 1] +
                     m.m[c * 4 + 2] * m.m[c * 4 + 2]);
}

} // namespace

TEST(ScaledRig, BoneWorldsMatchUnitysMeasuredMatrices)
{
    const std::string path = find_scene("SampleScene.p2b");
    if (path.empty()) {
        GTEST_SKIP() << "SampleScene.p2b not staged in build/";
    }
    std::FILE* f = std::fopen(path.c_str(), "rb");
    ASSERT_NE(f, nullptr);
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    static std::vector<uint8_t> bytes;
    bytes.resize(static_cast<size_t>(size));
    ASSERT_EQ(std::fread(bytes.data(), 1, bytes.size(), f), bytes.size());
    std::fclose(f);

    io::P2bFile file;
    ASSERT_TRUE(file.parse(bytes.data(), static_cast<uint32_t>(bytes.size())));
    static World world;
    ASSERT_TRUE(world.load(file)) << world.error();
    if (world.skinned_renderer_count() == 0) {
        GTEST_SKIP() << "staged scene has no skinned character";
    }

    world.update_animators(0.0f);
    const anim::Animator& animator =
        world.animator(world.skinned_renderer(0).animator);
    ASSERT_TRUE(animator.valid());

    // Bone 0 is the rig root ('spine'). Unity measured it at
    // (0.00, 1.00, -0.02) with lossy scale 12.25, relative to the Animator
    // (rigdiag, 2026-08-03). The default state's clip holds the bind pose
    // at t=0, so the composed bone world must land on those numbers.
    const Mat4& spine = animator.bone_world(0);
    EXPECT_NEAR(spine.m[12], 0.0f, 0.08f);
    EXPECT_NEAR(spine.m[13], 1.0f, 0.08f);
    EXPECT_NEAR(spine.m[14], -0.02f, 0.08f);
    EXPECT_NEAR(column_scale(spine, 0), 12.25f, 0.25f);
    EXPECT_NEAR(column_scale(spine, 1), 12.25f, 0.25f);
    EXPECT_NEAR(column_scale(spine, 2), 12.25f, 0.25f);
}
