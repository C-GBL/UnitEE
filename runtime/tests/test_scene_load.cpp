// M10 task 5: async and additive scene loading over the stream queue.
// Uses the real .p2b files the exporter produced, so the test covers the
// container path end to end rather than a synthetic buffer.
#include "ps2ur/scene_load.h"
#include "ps2ur/stream.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ps2ur;
using namespace ps2ur::scene;

namespace {

// The exported scenes live in build/ next to the tests. Skipping when they
// are absent keeps a fresh clone's test run green; the on-target sample
// covers the same path with files guaranteed present.
bool file_exists(const char* path)
{
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        return false;
    }
    std::fclose(f);
    return true;
}

// ctest runs from the build directory, so look for the exported scenes both
// there and at the repository root. An empty result means "not exported",
// which the tests skip on rather than fail.
std::string find_scene(const char* name)
{
    // ctest runs the binary from build/<preset>/runtime/tests, so walk up
    // far enough to find the repository's build/ directory from there.
    const std::string roots[] = {
        "build/",         "../build/",          "../../build/",
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

} // namespace

TEST(SceneLoad, AsyncLoadProgressesInStepsAndLandsReady)
{
    const std::string scene = find_scene("m5-scene.p2b");
    if (scene.empty()) {
        GTEST_SKIP() << "m5-scene.p2b not exported";
    }
    const char* kScenePath = scene.c_str();
    stream::shutdown();
    stream::init();

    static std::vector<uint8_t> buffer(4 * 1024 * 1024);
    static World world;
    SceneLoader loader;

    ASSERT_TRUE(loader.begin(kScenePath, buffer.data(),
                             static_cast<uint32_t>(buffer.size()), &world,
                             /*additive=*/false));
    EXPECT_EQ(loader.state(), LoadState::Reading);
    EXPECT_LT(loader.progress(), 1.0f);

    // Small budgets so the load genuinely spans several "frames" -- the
    // whole point of doing it asynchronously.
    uint32_t frames = 0;
    while (loader.state() == LoadState::Reading && frames < 10000u) {
        loader.update(16 * 1024);
        ++frames;
    }
    EXPECT_EQ(loader.state(), LoadState::Ready) << loader.error();
    EXPECT_GT(frames, 1u) << "a load that finishes in one step is not async";
    EXPECT_FLOAT_EQ(loader.progress(), 1.0f);
    EXPECT_GT(world.entity_count(), 0u);
    stream::shutdown();
}

TEST(SceneLoad, BlockingLoadProducesTheSameWorld)
{
    const std::string scene = find_scene("m5-scene.p2b");
    if (scene.empty()) {
        GTEST_SKIP() << "m5-scene.p2b not exported";
    }
    const char* kScenePath = scene.c_str();
    stream::shutdown();
    stream::init();

    static std::vector<uint8_t> buffer(4 * 1024 * 1024);
    static World world;
    SceneLoader loader;
    ASSERT_TRUE(loader.load_blocking(kScenePath, buffer.data(),
                                     static_cast<uint32_t>(buffer.size()),
                                     &world, false));
    EXPECT_EQ(loader.state(), LoadState::Ready);
    EXPECT_GT(world.entity_count(), 0u);
    EXPECT_GT(world.mesh_count(), 0u);
    stream::shutdown();
}

TEST(SceneLoad, AdditiveLoadRebasesIndicesOntoTheRunningWorld)
{
    const std::string scene = find_scene("m5-scene.p2b");
    const std::string extra = find_scene("spin-scene.p2b");
    if (scene.empty() || extra.empty()) {
        GTEST_SKIP() << "exported scenes not present";
    }
    const char* kScenePath = scene.c_str();
    const char* kSecondScene = extra.c_str();
    stream::shutdown();
    stream::init();

    static std::vector<uint8_t> first(4 * 1024 * 1024);
    static std::vector<uint8_t> second(4 * 1024 * 1024);
    static World world;

    SceneLoader base;
    ASSERT_TRUE(base.load_blocking(kScenePath, first.data(),
                                   static_cast<uint32_t>(first.size()), &world,
                                   false));
    const uint32_t entities_before = world.entity_count();
    const uint32_t meshes_before = world.mesh_count();
    ASSERT_GT(entities_before, 0u);

    SceneLoader additive;
    ASSERT_TRUE(additive.load_blocking(kSecondScene, second.data(),
                                       static_cast<uint32_t>(second.size()),
                                       &world, /*additive=*/true))
        << additive.error();

    EXPECT_GT(world.entity_count(), entities_before);
    EXPECT_GE(world.mesh_count(), meshes_before);

    // Every appended entity's references must point inside the MERGED world,
    // not at the indices they had in their own file.
    for (uint32_t i = entities_before; i < world.entity_count(); ++i) {
        const Entity& entity = world.entity(i);
        if (entity.parent >= 0) {
            EXPECT_GE(static_cast<uint32_t>(entity.parent), entities_before)
                << "appended entity " << i << " parented into the old scene";
            EXPECT_LT(static_cast<uint32_t>(entity.parent), world.entity_count());
        }
        if (entity.mesh >= 0) {
            EXPECT_LT(static_cast<uint32_t>(entity.mesh), world.mesh_count());
        }
    }
    stream::shutdown();
}

// Additive loading has to bring the animation side with it, not just
// geometry. A scene whose characters arrive without their skeletons, clips
// or controllers looks loaded and then stands perfectly still -- which is
// the failure mode this asserts against.
TEST(SceneLoad, AdditiveLoadCarriesSkinnedCharactersAndTheirAnimation)
{
    const std::string base = find_scene("spin-scene.p2b");
    const std::string skinned = find_scene("skinscene.p2b");
    if (base.empty() || skinned.empty()) {
        GTEST_SKIP() << "exported scenes not present";
    }
    stream::shutdown();
    stream::init();

    static std::vector<uint8_t> first(4 * 1024 * 1024);
    static std::vector<uint8_t> second(4 * 1024 * 1024);
    static World world;

    SceneLoader boot;
    ASSERT_TRUE(boot.load_blocking(base.c_str(), first.data(),
                                   static_cast<uint32_t>(first.size()), &world,
                                   false))
        << boot.error();
    ASSERT_EQ(world.skinned_renderer_count(), 0u)
        << "pick a base scene with no characters, or this proves nothing";

    SceneLoader additive;
    ASSERT_TRUE(additive.load_blocking(skinned.c_str(), second.data(),
                                       static_cast<uint32_t>(second.size()),
                                       &world, /*additive=*/true))
        << additive.error();

    ASSERT_GT(world.skinned_renderer_count(), 0u)
        << "additive load dropped the skinned renderers";
    ASSERT_GT(world.clip_count(), 0u);
    ASSERT_GT(world.skeleton_count(), 0u);

    for (uint32_t i = 0; i < world.skinned_renderer_count(); ++i) {
        const SkinnedRenderer& r = world.skinned_renderer(i);
        EXPECT_GE(r.entity, 0);
        EXPECT_LT(static_cast<uint32_t>(r.entity), world.entity_count());
        EXPECT_LT(static_cast<uint32_t>(r.mesh), world.skinned_mesh_count());
        EXPECT_LT(r.skeleton, world.skeleton_count());
        EXPECT_LT(r.controller, world.controller_count());
        // Every state must name a clip that exists in the MERGED clip table.
        const anim::Controller& c = world.controller(r.controller);
        for (uint32_t s = 0; s < c.state_count; ++s) {
            EXPECT_LT(static_cast<uint32_t>(c.states[s].clip), world.clip_count())
                << "controller state " << s << " kept a file-relative clip index";
        }
        EXPECT_TRUE(world.animator(r.animator).valid());
    }

    // And the characters must actually animate. A merged animator that is
    // "valid" but reads the wrong clips would hold the rest pose forever, so
    // assert the pose genuinely changes as time advances.
    const uint32_t slot = world.skinned_renderer(0).animator;
    const uint32_t bones = world.skeleton(world.skinned_renderer(0).skeleton).bone_count;
    ASSERT_GT(bones, 1u);
    world.update_animators(0.0f);
    const anim::Pose start = world.animator(slot).pose();
    bool moved = false;
    for (int step = 0; step < 30 && !moved; ++step) {
        world.update_animators(1.0f / 30.0f);
        const anim::Pose& now = world.animator(slot).pose();
        for (uint32_t b = 0; b < bones && !moved; ++b) {
            if (length(sub(now.pos[b], start.pos[b])) > 1e-4f ||
                fabsf(now.rot[b].w - start.rot[b].w) > 1e-4f) {
                moved = true;
            }
        }
    }
    EXPECT_TRUE(moved) << "the merged animator never left the rest pose";
    stream::shutdown();
}

// Loading a scene into a world that is already running -- the ordinary
// LoadScene("level2") path -- must REPLACE what is there, not accumulate on
// top of it. The animation tables are the ones that used to leak.
TEST(SceneLoad, ANonAdditiveLoadReplacesTheWorldIncludingItsAnimation)
{
    const std::string skinned = find_scene("skinscene.p2b");
    const std::string plain = find_scene("m5-scene.p2b");
    if (skinned.empty() || plain.empty()) {
        GTEST_SKIP() << "exported scenes not present";
    }
    stream::shutdown();
    stream::init();

    static std::vector<uint8_t> a(4 * 1024 * 1024);
    static std::vector<uint8_t> b(4 * 1024 * 1024);

    // What a fresh world looks like after loading each scene, for reference.
    static World fresh_skinned;
    static World fresh_plain;
    SceneLoader l1, l2;
    ASSERT_TRUE(l1.load_blocking(skinned.c_str(), a.data(),
                                 static_cast<uint32_t>(a.size()),
                                 &fresh_skinned, false))
        << l1.error();
    ASSERT_TRUE(l2.load_blocking(plain.c_str(), b.data(),
                                 static_cast<uint32_t>(b.size()), &fresh_plain,
                                 false))
        << l2.error();
    ASSERT_GT(fresh_skinned.skinned_renderer_count(), 0u);

    // Now the same two loads back to back into ONE world. The end state must
    // match the fresh load of whichever scene was loaded last.
    static World reused;
    SceneLoader r1, r2;
    ASSERT_TRUE(r1.load_blocking(skinned.c_str(), a.data(),
                                 static_cast<uint32_t>(a.size()), &reused, false))
        << r1.error();
    ASSERT_TRUE(r2.load_blocking(plain.c_str(), b.data(),
                                 static_cast<uint32_t>(b.size()), &reused, false))
        << r2.error();

    EXPECT_EQ(reused.entity_count(), fresh_plain.entity_count());
    EXPECT_EQ(reused.mesh_count(), fresh_plain.mesh_count());
    EXPECT_EQ(reused.material_count(), fresh_plain.material_count());
    EXPECT_EQ(reused.skinned_renderer_count(),
              fresh_plain.skinned_renderer_count())
        << "the previous scene's characters survived the reload";
    EXPECT_EQ(reused.skinned_mesh_count(), fresh_plain.skinned_mesh_count());
    EXPECT_EQ(reused.skeleton_count(), fresh_plain.skeleton_count());
    EXPECT_EQ(reused.clip_count(), fresh_plain.clip_count());
    EXPECT_EQ(reused.controller_count(), fresh_plain.controller_count());
    stream::shutdown();
}

// Three loads in a row is where a leaked scratch world shows up: the second
// merge inherits the first one's tables and the third overflows them.
TEST(SceneLoad, RepeatedAdditiveLoadsDoNotAccumulateInTheScratchWorld)
{
    const std::string plain = find_scene("m5-scene.p2b");
    const std::string spin = find_scene("spin-scene.p2b");
    const std::string skinned = find_scene("skinscene.p2b");
    if (plain.empty() || spin.empty() || skinned.empty()) {
        GTEST_SKIP() << "exported scenes not present";
    }
    stream::shutdown();
    stream::init();

    static std::vector<uint8_t> a(4 * 1024 * 1024);
    static std::vector<uint8_t> b(4 * 1024 * 1024);
    static std::vector<uint8_t> c(4 * 1024 * 1024);
    static World world;

    SceneLoader boot, add1, add2;
    ASSERT_TRUE(boot.load_blocking(plain.c_str(), a.data(),
                                   static_cast<uint32_t>(a.size()), &world,
                                   false))
        << boot.error();
    ASSERT_TRUE(add1.load_blocking(skinned.c_str(), b.data(),
                                   static_cast<uint32_t>(b.size()), &world, true))
        << add1.error();
    const uint32_t skinned_after_first = world.skinned_renderer_count();
    ASSERT_GT(skinned_after_first, 0u);

    // spin-scene has no characters, so this must not add any.
    ASSERT_TRUE(add2.load_blocking(spin.c_str(), c.data(),
                                   static_cast<uint32_t>(c.size()), &world, true))
        << add2.error();
    EXPECT_EQ(world.skinned_renderer_count(), skinned_after_first)
        << "a character-free scene added characters";
    stream::shutdown();
}

// AsyncOperation.allowSceneActivation: the read finishes, the bar parks at
// 0.9, and the world is NOT touched until activation is granted. The whole
// point is that a game can hold the swap until its fade-out is done, so the
// test asserts the world is genuinely unchanged while held.
TEST(SceneLoad, WithholdingActivationParksTheLoadWithoutSwappingTheWorld)
{
    const std::string scene = find_scene("m5-scene.p2b");
    if (scene.empty()) {
        GTEST_SKIP() << "m5-scene.p2b not exported";
    }
    stream::shutdown();
    stream::init();

    static std::vector<uint8_t> buffer(4 * 1024 * 1024);
    static World world;
    SceneLoader loader;
    ASSERT_TRUE(loader.begin(scene.c_str(), buffer.data(),
                             static_cast<uint32_t>(buffer.size()), &world,
                             false));
    EXPECT_TRUE(loader.allow_activation()) << "begin must reset to Unity's default";
    loader.set_allow_activation(false);

    uint32_t guard = 0;
    while (loader.state() == LoadState::Reading && guard++ < 10000u) {
        loader.update(16 * 1024);
    }
    ASSERT_EQ(loader.state(), LoadState::Parsing) << loader.error();
    EXPECT_FLOAT_EQ(loader.progress(), 0.9f);
    EXPECT_EQ(world.entity_count(), 0u) << "world swapped while activation was held";

    // Held state must be stable: more update() calls change nothing.
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(loader.update(16 * 1024), LoadState::Parsing);
    }
    EXPECT_EQ(world.entity_count(), 0u);

    loader.set_allow_activation(true);
    EXPECT_EQ(loader.update(16 * 1024), LoadState::Ready) << loader.error();
    EXPECT_FLOAT_EQ(loader.progress(), 1.0f);
    EXPECT_GT(world.entity_count(), 0u);
    stream::shutdown();
}

TEST(SceneLoad, AMissingFileFailsCleanly)
{
    stream::shutdown();
    stream::init();
    static std::vector<uint8_t> buffer(64 * 1024);
    static World world;
    SceneLoader loader;
    ASSERT_TRUE(loader.begin("does/not/exist.p2b", buffer.data(),
                             static_cast<uint32_t>(buffer.size()), &world,
                             false));
    uint32_t guard = 0;
    while (loader.state() == LoadState::Reading && guard++ < 1000u) {
        loader.update(16 * 1024);
    }
    EXPECT_EQ(loader.state(), LoadState::Failed);
    EXPECT_STRNE(loader.error(), "");
    stream::shutdown();
}

TEST(SceneLoad, BadArgumentsAreRefusedRatherThanCrashing)
{
    static World world;
    SceneLoader loader;
    std::vector<uint8_t> buffer(1024);
    EXPECT_FALSE(loader.begin(nullptr, buffer.data(), 1024, &world, false));
    EXPECT_FALSE(loader.begin("x.p2b", nullptr, 1024, &world, false));
    EXPECT_FALSE(loader.begin("x.p2b", buffer.data(), 0, &world, false));
    EXPECT_FALSE(loader.begin("x.p2b", buffer.data(), 1024, nullptr, false));
}
