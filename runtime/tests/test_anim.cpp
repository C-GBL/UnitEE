// M9: bone partitioning (the plan calls it out explicitly -- "a real
// algorithm, not an afterthought; write it with tests"), quantised clip
// sampling with forward-only cursors, slerp, and blending.
#include "ps2ur/anim.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace ps2ur;
using namespace ps2ur::anim;

namespace {

TriangleBones tri(std::initializer_list<uint16_t> bones)
{
    TriangleBones t{};
    for (uint16_t b : bones) {
        t.bone[t.count++] = b;
    }
    return t;
}

// Builds a 12-byte key record: time in [0,1] of the clip, three or four
// quantised components.
void put_key(std::vector<uint8_t>& out, float normalized_time, float x, float y,
             float z, float w, float quant_scale)
{
    const uint16_t t = static_cast<uint16_t>(normalized_time * 65535.0f + 0.5f);
    out.push_back(static_cast<uint8_t>(t & 0xFF));
    out.push_back(static_cast<uint8_t>(t >> 8));
    out.push_back(0);
    out.push_back(0);
    const float inv = quant_scale != 0.0f ? 32767.0f / quant_scale : 0.0f;
    const float values[4] = {x, y, z, w};
    for (int i = 0; i < 4; ++i) {
        float scaled = values[i] * inv;
        if (scaled > 32767.0f) scaled = 32767.0f;
        if (scaled < -32767.0f) scaled = -32767.0f;
        const int16_t q = static_cast<int16_t>(scaled >= 0.0f ? scaled + 0.5f
                                                              : scaled - 0.5f);
        out.push_back(static_cast<uint8_t>(static_cast<uint16_t>(q) & 0xFF));
        out.push_back(static_cast<uint8_t>(static_cast<uint16_t>(q) >> 8));
    }
}

Skeleton two_bone_skeleton()
{
    Skeleton s{};
    s.bone_count = 2;
    s.bones[0].parent = -1;
    s.bones[0].rest_pos = Vec3{0, 0, 0};
    s.bones[0].rest_rot = quat_identity();
    s.bones[0].rest_scale = Vec3{1, 1, 1};
    s.bones[1].parent = 0;
    s.bones[1].rest_pos = Vec3{0, 1, 0};
    s.bones[1].rest_rot = quat_identity();
    s.bones[1].rest_scale = Vec3{1, 1, 1};
    return s;
}

} // namespace

// ---- partitioning ---------------------------------------------------------

TEST(BonePartition, EverythingFitsInOneBatch)
{
    const TriangleBones tris[] = {tri({0, 1, 2}), tri({1, 2, 3}), tri({0, 3})};
    PartitionBatch batches[8];
    const uint32_t n = partition_triangles(tris, 3, 24, 100, batches, 8);
    ASSERT_EQ(n, 1u);
    EXPECT_EQ(batches[0].triangle_count, 3u);
    EXPECT_EQ(batches[0].bone_count, 4u); // 0,1,2,3 -- deduplicated
}

TEST(BonePartition, SplitsWhenThePaletteWouldOverflow)
{
    // Palette of 4; each triangle brings two fresh bones.
    std::vector<TriangleBones> tris;
    for (uint16_t i = 0; i < 6; ++i) {
        tris.push_back(tri({static_cast<uint16_t>(i * 2),
                            static_cast<uint16_t>(i * 2 + 1)}));
    }
    PartitionBatch batches[16];
    const uint32_t n = partition_triangles(tris.data(), 6, 4, 100, batches, 16);
    ASSERT_EQ(n, 3u); // two triangles (4 bones) per batch
    for (uint32_t b = 0; b < n; ++b) {
        EXPECT_LE(batches[b].bone_count, 4u);
        EXPECT_EQ(batches[b].triangle_count, 2u);
    }
}

TEST(BonePartition, CoversEveryTriangleExactlyOnceAndContiguously)
{
    std::vector<TriangleBones> tris;
    for (uint16_t i = 0; i < 40; ++i) {
        tris.push_back(tri({static_cast<uint16_t>(i % 30),
                            static_cast<uint16_t>((i * 7) % 30),
                            static_cast<uint16_t>((i * 13) % 30)}));
    }
    PartitionBatch batches[64];
    const uint32_t n =
        partition_triangles(tris.data(), 40, 8, 100, batches, 64);
    ASSERT_GT(n, 0u);

    uint32_t expected_first = 0;
    uint32_t total = 0;
    for (uint32_t b = 0; b < n; ++b) {
        EXPECT_EQ(batches[b].first_triangle, expected_first);
        EXPECT_GT(batches[b].triangle_count, 0u);
        EXPECT_LE(batches[b].bone_count, 8u);
        // Every bone the batch's triangles use must be in its table.
        for (uint32_t t = 0; t < batches[b].triangle_count; ++t) {
            const TriangleBones& source = tris[batches[b].first_triangle + t];
            for (uint32_t k = 0; k < source.count; ++k) {
                bool found = false;
                for (uint32_t s = 0; s < batches[b].bone_count && !found; ++s) {
                    found = batches[b].bones[s] == source.bone[k];
                }
                EXPECT_TRUE(found) << "batch " << b << " missing bone "
                                   << source.bone[k];
            }
        }
        expected_first += batches[b].triangle_count;
        total += batches[b].triangle_count;
    }
    EXPECT_EQ(total, 40u);
}

TEST(BonePartition, HonoursTheTriangleBudget)
{
    std::vector<TriangleBones> tris(20, tri({0, 1, 2}));
    PartitionBatch batches[32];
    const uint32_t n = partition_triangles(tris.data(), 20, 24, 6, batches, 32);
    ASSERT_EQ(n, 4u); // 6 + 6 + 6 + 2
    EXPECT_EQ(batches[0].triangle_count, 6u);
    EXPECT_EQ(batches[3].triangle_count, 2u);
}

TEST(BonePartition, RejectsATriangleThatCannotEverFit)
{
    const TriangleBones tris[] = {tri({0, 1, 2, 3, 4, 5})};
    PartitionBatch batches[4];
    // Palette of 4 cannot hold a 6-bone triangle: no partition exists.
    EXPECT_EQ(partition_triangles(tris, 1, 4, 100, batches, 4), 0u);
}

TEST(BonePartition, RejectsWhenTheOutputArrayIsTooSmall)
{
    std::vector<TriangleBones> tris;
    for (uint16_t i = 0; i < 10; ++i) {
        tris.push_back(tri({static_cast<uint16_t>(i * 2),
                            static_cast<uint16_t>(i * 2 + 1)}));
    }
    PartitionBatch batches[2];
    EXPECT_EQ(partition_triangles(tris.data(), 10, 2, 100, batches, 2), 0u);
}

TEST(BonePartition, ValidatorAcceptsAWellFormedBatch)
{
    const uint16_t table[] = {3, 7, 11};
    const uint8_t slots[] = {0, 1, 2, 2, 0};
    EXPECT_TRUE(validate_partition(table, 3, 16, slots, 5, 24));
}

TEST(BonePartition, ValidatorRejectsMalformedBatches)
{
    const uint8_t slots[] = {0, 1};
    const uint16_t duplicate[] = {4, 4};
    EXPECT_FALSE(validate_partition(duplicate, 2, 16, slots, 2, 24));

    const uint16_t out_of_range[] = {4, 99};
    EXPECT_FALSE(validate_partition(out_of_range, 2, 16, slots, 2, 24));

    const uint16_t fine[] = {4, 5};
    const uint8_t bad_slot[] = {0, 2}; // slot 2 with a 2-entry table
    EXPECT_FALSE(validate_partition(fine, 2, 16, bad_slot, 2, 24));

    // Table larger than the palette.
    uint16_t oversize[32];
    for (uint16_t i = 0; i < 32; ++i) {
        oversize[i] = i;
    }
    EXPECT_FALSE(validate_partition(oversize, 32, 64, slots, 2, 24));
}

// ---- sampling -------------------------------------------------------------

TEST(ClipSampling, DequantisesAndInterpolatesTranslation)
{
    std::vector<uint8_t> keys;
    put_key(keys, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 4.0f);
    put_key(keys, 1.0f, 4.0f, -2.0f, 0.0f, 0.0f, 4.0f);

    Clip clip{};
    clip.duration = 2.0f;
    clip.loop = false;
    clip.keys = keys.data();
    clip.key_count = 2;
    clip.track_count = 1;
    clip.tracks[0].bone = 1;
    clip.tracks[0].channel = kChannelTranslation;
    clip.tracks[0].key_count = 2;
    clip.tracks[0].key_first = 0;
    clip.tracks[0].quant_scale = 4.0f;

    const Skeleton skeleton = two_bone_skeleton();
    Pose pose{};
    rest_pose(skeleton, pose);

    ClipPlayer player;
    player.set_clip(&clip, 1.0f, false);
    player.sample(skeleton, pose);
    EXPECT_NEAR(pose.pos[1].x, 0.0f, 1e-3f);

    player.set_time(1.0f); // halfway
    player.sample(skeleton, pose);
    EXPECT_NEAR(pose.pos[1].x, 2.0f, 1e-3f);
    EXPECT_NEAR(pose.pos[1].y, -1.0f, 1e-3f);

    player.set_time(2.0f);
    player.sample(skeleton, pose);
    EXPECT_NEAR(pose.pos[1].x, 4.0f, 1e-3f);
    EXPECT_NEAR(pose.pos[1].y, -2.0f, 1e-3f);
}

TEST(ClipSampling, QuantisationErrorStaysWithinHalfAStep)
{
    // 16-bit over a +-8 range: one step is 8/32767, so a round-trip must
    // land within half a step. Sampling happens at each key's EXACT stored
    // time so this measures value quantisation alone -- key times are
    // quantised too (u16 over the duration), and sampling at a nominal time
    // like 3.0 would interpolate toward the next key and fold that error in.
    const float range = 8.0f;
    const float step = range / 32767.0f;
    const float duration = 4.0f;
    std::vector<uint8_t> keys;
    const float samples[] = {0.0f, 0.37f, -1.9f, 7.99f, -7.99f};
    for (int i = 0; i < 5; ++i) {
        put_key(keys, static_cast<float>(i) / 4.0f, samples[i], 0, 0, 0, range);
    }

    Clip clip{};
    clip.duration = duration;
    clip.keys = keys.data();
    clip.key_count = 5;
    clip.track_count = 1;
    clip.tracks[0].bone = 0;
    clip.tracks[0].channel = kChannelTranslation;
    clip.tracks[0].key_count = 5;
    clip.tracks[0].quant_scale = range;

    const Skeleton skeleton = two_bone_skeleton();
    Pose pose{};
    ClipPlayer player;
    player.set_clip(&clip, 1.0f, false);
    for (int i = 0; i < 5; ++i) {
        const uint16_t stored =
            static_cast<uint16_t>(static_cast<float>(i) / 4.0f * 65535.0f + 0.5f);
        player.set_time(static_cast<float>(stored) / 65535.0f * duration);
        player.sample(skeleton, pose);
        EXPECT_LT(std::fabs(pose.pos[0].x - samples[i]), step * 0.51f)
            << "sample " << i;
    }
}

TEST(ClipSampling, KeyTimeQuantisationStaysBelowOneMillisecond)
{
    // Times are u16 fractions of the duration: a 5-second clip (the M9
    // acceptance length) resolves to 5/65535 s = 76 microseconds, so no
    // key lands more than that from where the exporter put it.
    const float duration = 5.0f;
    const float tick = duration / 65535.0f;
    EXPECT_LT(tick, 0.001f);
}

TEST(ClipSampling, CursorWalksForwardWithoutRescanning)
{
    // 64 keys stepping x by 1 each: playing forward must land on the right
    // value at every step, which only holds if the cursor advances with time.
    std::vector<uint8_t> keys;
    for (int i = 0; i < 64; ++i) {
        put_key(keys, static_cast<float>(i) / 63.0f, static_cast<float>(i), 0, 0,
                0, 64.0f);
    }
    Clip clip{};
    clip.duration = 63.0f;
    clip.keys = keys.data();
    clip.key_count = 64;
    clip.track_count = 1;
    clip.tracks[0].bone = 0;
    clip.tracks[0].channel = kChannelTranslation;
    clip.tracks[0].key_count = 64;
    clip.tracks[0].quant_scale = 64.0f;

    const Skeleton skeleton = two_bone_skeleton();
    Pose pose{};
    ClipPlayer player;
    player.set_clip(&clip, 1.0f, false);
    for (int i = 0; i < 63; ++i) {
        player.advance(1.0f);
        player.sample(skeleton, pose);
        EXPECT_NEAR(pose.pos[0].x, static_cast<float>(i + 1), 0.01f)
            << "step " << i;
    }
}

TEST(ClipSampling, LoopingWrapsTimeAndResumesFromTheStart)
{
    std::vector<uint8_t> keys;
    put_key(keys, 0.0f, 0.0f, 0, 0, 0, 10.0f);
    put_key(keys, 1.0f, 10.0f, 0, 0, 0, 10.0f);

    Clip clip{};
    clip.duration = 1.0f;
    clip.loop = true;
    clip.keys = keys.data();
    clip.key_count = 2;
    clip.track_count = 1;
    clip.tracks[0].bone = 0;
    clip.tracks[0].channel = kChannelTranslation;
    clip.tracks[0].key_count = 2;
    clip.tracks[0].quant_scale = 10.0f;

    const Skeleton skeleton = two_bone_skeleton();
    Pose pose{};
    ClipPlayer player;
    player.set_clip(&clip, 1.0f, true);

    player.advance(0.75f);
    player.sample(skeleton, pose);
    EXPECT_NEAR(pose.pos[0].x, 7.5f, 0.05f);

    // Crossing the end wraps rather than clamping, and sampling after the
    // wrap must give the early value again (cursor rewound).
    EXPECT_FALSE(player.advance(0.5f));
    EXPECT_NEAR(player.time(), 0.25f, 1e-4f);
    player.sample(skeleton, pose);
    EXPECT_NEAR(pose.pos[0].x, 2.5f, 0.05f);
}

TEST(ClipSampling, NonLoopingClipReportsItsEndExactlyOnce)
{
    std::vector<uint8_t> keys;
    put_key(keys, 0.0f, 0.0f, 0, 0, 0, 1.0f);
    put_key(keys, 1.0f, 1.0f, 0, 0, 0, 1.0f);
    Clip clip{};
    clip.duration = 1.0f;
    clip.keys = keys.data();
    clip.key_count = 2;
    clip.track_count = 1;
    clip.tracks[0].key_count = 2;
    clip.tracks[0].quant_scale = 1.0f;

    ClipPlayer player;
    player.set_clip(&clip, 1.0f, false);
    EXPECT_FALSE(player.advance(0.5f));
    EXPECT_TRUE(player.advance(0.75f));
    EXPECT_NEAR(player.time(), 1.0f, 1e-5f);
}

TEST(ClipSampling, RotationTracksSlerpBetweenKeys)
{
    const Quat a = quat_identity();
    const Quat b = quat_from_axis_angle(Vec3{0, 1, 0}, kPi * 0.5f);
    std::vector<uint8_t> keys;
    put_key(keys, 0.0f, a.x, a.y, a.z, a.w, 1.0f);
    put_key(keys, 1.0f, b.x, b.y, b.z, b.w, 1.0f);

    Clip clip{};
    clip.duration = 1.0f;
    clip.keys = keys.data();
    clip.key_count = 2;
    clip.track_count = 1;
    clip.tracks[0].bone = 0;
    clip.tracks[0].channel = kChannelRotation;
    clip.tracks[0].key_count = 2;
    clip.tracks[0].quant_scale = 1.0f;

    const Skeleton skeleton = two_bone_skeleton();
    Pose pose{};
    ClipPlayer player;
    player.set_clip(&clip, 1.0f, false);
    player.set_time(0.5f);
    player.sample(skeleton, pose);

    // Halfway between identity and a 90-degree turn is 45 degrees.
    const Quat expected = quat_from_axis_angle(Vec3{0, 1, 0}, kPi * 0.25f);
    EXPECT_NEAR(std::fabs(quat_dot(pose.rot[0], expected)), 1.0f, 1e-3f);
}

// ---- blending -------------------------------------------------------------

TEST(Blending, SlerpTakesTheShortArc)
{
    const Quat a = quat_from_axis_angle(Vec3{0, 1, 0}, 0.1f);
    // Same rotation, negated representation: the long way is 2pi minus the
    // short way, and a naive lerp would travel it.
    const Quat b_far = quat_from_axis_angle(Vec3{0, 1, 0}, 0.3f);
    const Quat b = Quat{-b_far.x, -b_far.y, -b_far.z, -b_far.w};

    const Quat mid = quat_slerp(a, b, 0.5f);
    const Quat expected = quat_from_axis_angle(Vec3{0, 1, 0}, 0.2f);
    EXPECT_NEAR(std::fabs(quat_dot(mid, expected)), 1.0f, 1e-4f);
}

TEST(Blending, SlerpEndpointsAreExact)
{
    const Quat a = quat_from_axis_angle(Vec3{1, 0, 0}, 0.4f);
    const Quat b = quat_from_axis_angle(Vec3{0, 0, 1}, 1.1f);
    EXPECT_NEAR(std::fabs(quat_dot(quat_slerp(a, b, 0.0f), a)), 1.0f, 1e-5f);
    EXPECT_NEAR(std::fabs(quat_dot(quat_slerp(a, b, 1.0f), b)), 1.0f, 1e-5f);
}

TEST(Blending, PoseBlendInterpolatesAndRespectsTheMask)
{
    const Skeleton skeleton = two_bone_skeleton();
    Pose a{};
    Pose b{};
    rest_pose(skeleton, a);
    rest_pose(skeleton, b);
    a.pos[0] = Vec3{0, 0, 0};
    b.pos[0] = Vec3{10, 0, 0};
    a.pos[1] = Vec3{0, 0, 0};
    b.pos[1] = Vec3{10, 0, 0};

    Pose blended = a;
    blend_pose(blended, b, 0.25f, nullptr, 2);
    EXPECT_NEAR(blended.pos[0].x, 2.5f, 1e-5f);
    EXPECT_NEAR(blended.pos[1].x, 2.5f, 1e-5f);

    // Mask selecting bone 1 only leaves bone 0 untouched.
    uint32_t mask[kMaskWords] = {};
    mask[0] = 1u << 1;
    Pose masked = a;
    blend_pose(masked, b, 1.0f, mask, 2);
    EXPECT_NEAR(masked.pos[0].x, 0.0f, 1e-5f);
    EXPECT_NEAR(masked.pos[1].x, 10.0f, 1e-5f);
}

TEST(Blending, AdditiveWithNoDeltaLeavesTheBaseAlone)
{
    const Skeleton skeleton = two_bone_skeleton();
    Pose base{};
    Pose reference{};
    rest_pose(skeleton, base);
    rest_pose(skeleton, reference);
    base.pos[1] = Vec3{3, 4, 5};
    const Pose additive = reference; // no delta

    blend_additive(base, additive, reference, 1.0f, nullptr, 2);
    EXPECT_NEAR(base.pos[1].x, 3.0f, 1e-5f);
    EXPECT_NEAR(base.pos[1].y, 4.0f, 1e-5f);
    EXPECT_NEAR(base.pos[1].z, 5.0f, 1e-5f);
}

TEST(Blending, AdditiveAppliesAScaledDelta)
{
    const Skeleton skeleton = two_bone_skeleton();
    Pose base{};
    Pose reference{};
    rest_pose(skeleton, base);
    rest_pose(skeleton, reference);
    Pose additive = reference;
    additive.pos[1] = add(reference.pos[1], Vec3{4, 0, 0});

    blend_additive(base, additive, reference, 0.5f, nullptr, 2);
    EXPECT_NEAR(base.pos[1].x, reference.pos[1].x + 2.0f, 1e-5f);
}

// ---- palette --------------------------------------------------------------

TEST(Palette, BindPoseProducesIdentityMatrices)
{
    // With the pose equal to the bind pose, bone_world * inverse_bind is the
    // identity: a character in bind pose must render exactly as authored.
    Skeleton skeleton = two_bone_skeleton();
    Animator animator;
    Controller controller{};
    animator.bind(&skeleton, &controller, nullptr, 0);
    animator.compute_bone_world();
    for (uint32_t i = 0; i < skeleton.bone_count; ++i) {
        skeleton.bones[i].inverse_bind = mat4_rigid_inverse(animator.bone_world(i));
    }

    const uint16_t table[] = {0, 1};
    Mat4 palette[2];
    build_palette(animator, table, 2, palette);
    for (uint32_t s = 0; s < 2; ++s) {
        for (int i = 0; i < 16; ++i) {
            const float expected = (i % 5 == 0) ? 1.0f : 0.0f;
            EXPECT_NEAR(palette[s].m[i], expected, 1e-4f)
                << "slot " << s << " element " << i;
        }
    }
}

// ---- root motion and sockets ----------------------------------------------

namespace {

// A one-bone clip that walks the root forward 3 units over 3 seconds.
struct RootMotionFixture {
    std::vector<uint8_t> keys;
    Clip clip{};
    Controller controller{};
    Skeleton skeleton = two_bone_skeleton();

    RootMotionFixture()
    {
        put_key(keys, 0.0f, 0.0f, 0, 0, 0, 4.0f);
        put_key(keys, 1.0f, 3.0f, 0, 0, 0, 4.0f);
        clip.duration = 3.0f;
        clip.keys = keys.data();
        clip.key_count = 2;
        clip.track_count = 1;
        clip.tracks[0].bone = 0; // the root
        clip.tracks[0].channel = kChannelTranslation;
        clip.tracks[0].key_count = 2;
        clip.tracks[0].quant_scale = 4.0f;

        controller.state_count = 1;
        controller.states[0].clip = 0;
        controller.states[0].speed = 1.0f;
        controller.states[0].loop = false;
    }
};

} // namespace

TEST(RootMotion, DeltaIsExtractedAndTheRootStaysPinned)
{
    RootMotionFixture fixture;
    Animator animator;
    animator.bind(&fixture.skeleton, &fixture.controller, &fixture.clip, 1);
    animator.set_root_motion(true);

    animator.update(0.0f); // establishes the reference sample
    EXPECT_NEAR(animator.root_motion_delta().x, 0.0f, 1e-4f);

    // One second is a third of the clip: the root should advance ~1 unit and
    // the POSE's root must stay at its rest position, or the character would
    // travel twice (once by the pose, once by the entity).
    animator.update(1.0f);
    EXPECT_NEAR(animator.root_motion_delta().x, 1.0f, 0.01f);
    EXPECT_NEAR(animator.pose().pos[0].x, fixture.skeleton.bones[0].rest_pos.x,
                1e-4f);

    animator.update(1.0f);
    EXPECT_NEAR(animator.root_motion_delta().x, 1.0f, 0.01f);
}

TEST(RootMotion, DisabledLeavesTheMotionInThePose)
{
    RootMotionFixture fixture;
    Animator animator;
    animator.bind(&fixture.skeleton, &fixture.controller, &fixture.clip, 1);
    // Root motion off: the clip's translation stays in the pose and the
    // delta is always zero.
    animator.update(0.0f);
    animator.update(1.5f);
    EXPECT_NEAR(animator.root_motion_delta().x, 0.0f, 1e-6f);
    EXPECT_NEAR(animator.pose().pos[0].x, 1.5f, 0.01f);
}

TEST(Sockets, FollowTheirBoneThroughTheEntityTransform)
{
    Skeleton skeleton = two_bone_skeleton();
    Animator animator;
    Controller controller{};
    animator.bind(&skeleton, &controller, nullptr, 0);
    animator.compute_bone_world();

    // A socket 0.5 above bone 1 (which rests 1.0 above the root).
    Socket socket{};
    socket.bone = 1;
    socket.offset = mat4_translate(Vec3{0.0f, 0.5f, 0.0f});

    const Mat4 identity = mat4_identity();
    Mat4 world = socket_world(animator, socket, identity);
    EXPECT_NEAR(world.m[13], 1.5f, 1e-4f);

    // Move the entity: the socket moves with it.
    const Mat4 entity = mat4_translate(Vec3{10.0f, 0.0f, 0.0f});
    world = socket_world(animator, socket, entity);
    EXPECT_NEAR(world.m[12], 10.0f, 1e-4f);
    EXPECT_NEAR(world.m[13], 1.5f, 1e-4f);
}

TEST(StateMachine, TriggerTransitionsCrossfadeAndConsumeTheTrigger)
{
    RootMotionFixture fixture;
    // Two states over the same clip, with a trigger transition between them.
    fixture.controller.state_count = 2;
    fixture.controller.states[1].clip = 0;
    fixture.controller.states[1].speed = 1.0f;
    fixture.controller.states[1].loop = true;
    fixture.controller.transition_count = 1;
    fixture.controller.transitions[0].from = 0;
    fixture.controller.transitions[0].to = 1;
    fixture.controller.transitions[0].duration = 0.5f;
    fixture.controller.transitions[0].condition = kConditionTrigger;
    fixture.controller.transitions[0].param = 0;
    fixture.controller.param_count = 1;
    fixture.controller.param_hash[0] = 0x1234u;

    Animator animator;
    animator.bind(&fixture.skeleton, &fixture.controller, &fixture.clip, 1);
    EXPECT_EQ(animator.state(), 0u);

    animator.update(0.1f);
    EXPECT_EQ(animator.state(), 0u); // nothing fires without the trigger

    animator.set_trigger(0x1234u);
    animator.update(0.1f);
    EXPECT_EQ(animator.state(), 1u);
    EXPECT_TRUE(animator.blending());

    // The trigger was consumed: coming back needs a fresh one.
    for (int i = 0; i < 20; ++i) {
        animator.update(0.05f);
    }
    EXPECT_FALSE(animator.blending());
    EXPECT_EQ(animator.state(), 1u);
}

TEST(StateMachine, FloatConditionsFireWhenTheParameterCrosses)
{
    RootMotionFixture fixture;
    fixture.controller.state_count = 2;
    fixture.controller.states[1].clip = 0;
    fixture.controller.transition_count = 1;
    fixture.controller.transitions[0].from = 0;
    fixture.controller.transitions[0].to = 1;
    fixture.controller.transitions[0].duration = 0.25f;
    fixture.controller.transitions[0].condition = kConditionFloatGreater;
    fixture.controller.transitions[0].param = 0;
    fixture.controller.transitions[0].threshold = 0.5f;
    fixture.controller.param_count = 1;
    fixture.controller.param_hash[0] = 0xABCDu;

    Animator animator;
    animator.bind(&fixture.skeleton, &fixture.controller, &fixture.clip, 1);

    animator.set_float(0xABCDu, 0.25f);
    animator.update(0.1f);
    EXPECT_EQ(animator.state(), 0u);

    animator.set_float(0xABCDu, 0.75f);
    animator.update(0.1f);
    EXPECT_EQ(animator.state(), 1u);
}

TEST(Palette, OutOfRangeBonesFallBackToIdentity)
{
    Skeleton skeleton = two_bone_skeleton();
    Animator animator;
    Controller controller{};
    animator.bind(&skeleton, &controller, nullptr, 0);
    const uint16_t table[] = {99};
    Mat4 palette[1];
    build_palette(animator, table, 1, palette);
    EXPECT_NEAR(palette[0].m[0], 1.0f, 1e-6f);
    EXPECT_NEAR(palette[0].m[1], 0.0f, 1e-6f);
}

// ---- 1D blend trees (M12.5) -----------------------------------------------

namespace {

// Three constant-pose clips (root x = 0 / 2 / 6) on one state's blend tree
// with thresholds 0 / 0.5 / 1: the locomotion shape.
struct BlendTreeFixture {
    std::vector<uint8_t> keys0, keys1, keys2;
    Clip clips[3];
    Controller controller{};
    Skeleton skeleton = two_bone_skeleton();
    static constexpr uint32_t kSpeedHash = 0xC0FFEEu;

    static void constant_clip(Clip& clip, std::vector<uint8_t>& keys, float x)
    {
        put_key(keys, 0.0f, x, 0, 0, 0, 8.0f);
        put_key(keys, 1.0f, x, 0, 0, 0, 8.0f);
        clip.duration = 1.0f;
        clip.keys = keys.data();
        clip.key_count = 2;
        clip.track_count = 1;
        clip.tracks[0].bone = 0;
        clip.tracks[0].channel = kChannelTranslation;
        clip.tracks[0].key_count = 2;
        clip.tracks[0].quant_scale = 8.0f;
    }

    BlendTreeFixture()
    {
        constant_clip(clips[0], keys0, 0.0f);
        constant_clip(clips[1], keys1, 2.0f);
        constant_clip(clips[2], keys2, 6.0f);

        controller.state_count = 1;
        controller.states[0].clip = 0;
        controller.states[0].speed = 1.0f;
        controller.states[0].loop = true;
        controller.param_count = 1;
        controller.param_hash[0] = kSpeedHash;
        controller.tree_count = 1;
        controller.trees[0].state = 0;
        controller.trees[0].param = 0;
        controller.trees[0].child_count = 3;
        controller.trees[0].clip[0] = 0;
        controller.trees[0].clip[1] = 1;
        controller.trees[0].clip[2] = 2;
        controller.trees[0].threshold[0] = 0.0f;
        controller.trees[0].threshold[1] = 0.5f;
        controller.trees[0].threshold[2] = 1.0f;
    }
};

} // namespace

TEST(BlendTree1D, BlendsTheBracketingPairByTheParameter)
{
    BlendTreeFixture fixture;
    Animator animator;
    animator.bind(&fixture.skeleton, &fixture.controller, fixture.clips, 3);

    animator.set_float(BlendTreeFixture::kSpeedHash, 0.0f);
    animator.update(0.016f);
    EXPECT_NEAR(animator.pose().pos[0].x, 0.0f, 0.01f);

    // Midway through the FIRST segment: half of clips 0 and 1.
    animator.set_float(BlendTreeFixture::kSpeedHash, 0.25f);
    animator.update(0.016f);
    EXPECT_NEAR(animator.pose().pos[0].x, 1.0f, 0.02f);

    // Exactly at the middle threshold: pure clip 1.
    animator.set_float(BlendTreeFixture::kSpeedHash, 0.5f);
    animator.update(0.016f);
    EXPECT_NEAR(animator.pose().pos[0].x, 2.0f, 0.02f);

    // Midway through the SECOND segment: half of clips 1 and 2.
    animator.set_float(BlendTreeFixture::kSpeedHash, 0.75f);
    animator.update(0.016f);
    EXPECT_NEAR(animator.pose().pos[0].x, 4.0f, 0.03f);

    animator.set_float(BlendTreeFixture::kSpeedHash, 1.0f);
    animator.update(0.016f);
    EXPECT_NEAR(animator.pose().pos[0].x, 6.0f, 0.03f);
}

TEST(BlendTree1D, ClampsOutsideTheThresholdRange)
{
    BlendTreeFixture fixture;
    Animator animator;
    animator.bind(&fixture.skeleton, &fixture.controller, fixture.clips, 3);

    animator.set_float(BlendTreeFixture::kSpeedHash, -5.0f);
    animator.update(0.016f);
    EXPECT_NEAR(animator.pose().pos[0].x, 0.0f, 0.01f);

    animator.set_float(BlendTreeFixture::kSpeedHash, 42.0f);
    animator.update(0.016f);
    EXPECT_NEAR(animator.pose().pos[0].x, 6.0f, 0.03f);
}

TEST(BlendTree1D, LoopsAndKeepsPlayingAcrossParameterSweeps)
{
    BlendTreeFixture fixture;
    Animator animator;
    animator.bind(&fixture.skeleton, &fixture.controller, fixture.clips, 3);

    // Sweep the parameter while advancing past several loop lengths: the
    // pose must always be the blend the CURRENT parameter selects, with no
    // stalls or runaway values from the phase bookkeeping.
    for (int i = 0; i <= 40; ++i) {
        const float p = static_cast<float>(i % 11) / 10.0f;
        animator.set_float(BlendTreeFixture::kSpeedHash, p);
        animator.update(0.1f);
        const float expected =
            p <= 0.5f ? p * 4.0f : 2.0f + (p - 0.5f) * 8.0f;
        EXPECT_NEAR(animator.pose().pos[0].x, expected, 0.05f)
            << "param " << p << " at step " << i;
    }
}
