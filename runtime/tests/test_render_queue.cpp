// M8 tasks 3/4: render-queue key packing + radix ordering, and the frustum
// culling math. Sorting correctness is the transparent-object acceptance
// criterion in miniature, so it gets exact tests.
#include "ps2ur/math.h"
#include "ps2ur/render_queue.h"

#include <gtest/gtest.h>

using namespace ps2ur;
using gfx::RenderQueue;

TEST(RenderQueue, OpaqueSortsFrontToBackTransparentBackToFront)
{
    RenderQueue q;
    // Interleaved pushes; entity ids mark identity.
    ASSERT_TRUE(q.push(0, 0, 0, 0.9f, 1, 0, 0)); // opaque far
    ASSERT_TRUE(q.push(1, 3, 0, 0.2f, 2, 0, 0)); // transparent near
    ASSERT_TRUE(q.push(0, 0, 0, 0.1f, 3, 0, 0)); // opaque near
    ASSERT_TRUE(q.push(1, 3, 0, 0.8f, 4, 0, 0)); // transparent far
    q.sort();

    ASSERT_EQ(q.count(), 4u);
    EXPECT_EQ(q.command(0).entity, 3); // opaque near first
    EXPECT_EQ(q.command(1).entity, 1); // opaque far
    EXPECT_EQ(q.command(2).entity, 4); // transparent FAR first
    EXPECT_EQ(q.command(3).entity, 2); // transparent near last
}

TEST(RenderQueue, GroupsByKindThenTextureWithinAPass)
{
    RenderQueue q;
    ASSERT_TRUE(q.push(0, 2, 0, 0.5f, 1, 0, 0)); // lit untextured
    ASSERT_TRUE(q.push(0, 1, 2, 0.5f, 2, 0, 0)); // textured, tex 1
    ASSERT_TRUE(q.push(0, 1, 1, 0.5f, 3, 0, 0)); // textured, tex 0
    ASSERT_TRUE(q.push(0, 2, 0, 0.4f, 4, 0, 0)); // lit untextured, nearer
    q.sort();

    // kind ascending; within a kind, texture ascending; within that, depth.
    EXPECT_EQ(q.command(0).entity, 3);
    EXPECT_EQ(q.command(1).entity, 2);
    EXPECT_EQ(q.command(2).entity, 4);
    EXPECT_EQ(q.command(3).entity, 1);
}

TEST(RenderQueue, StableForEqualKeys)
{
    RenderQueue q;
    for (uint16_t i = 0; i < 8; ++i) {
        ASSERT_TRUE(q.push(0, 0, 0, 0.5f, i, 0, 0));
    }
    q.sort();
    for (uint16_t i = 0; i < 8; ++i) {
        EXPECT_EQ(q.command(i).entity, i);
    }
}

TEST(RenderQueue, DepthClampsInsteadOfWrapping)
{
    const uint64_t below = RenderQueue::make_key(0, 0, 0, -0.5f, 0);
    const uint64_t zero = RenderQueue::make_key(0, 0, 0, 0.0f, 0);
    const uint64_t one = RenderQueue::make_key(0, 0, 0, 1.0f, 0);
    const uint64_t above = RenderQueue::make_key(0, 0, 0, 1.5f, 0);
    EXPECT_EQ(below, zero);
    EXPECT_EQ(above, one);
    EXPECT_LT(zero, one);
}

TEST(Frustum, CullsSpheresOutsideAndKeepsSpheresInside)
{
    // Camera at origin looking down -z (RH), 90 degree fov, near 1 far 100.
    const Mat4 vp = mat4_perspective(1.5707963f, 1.0f, 1.0f, 100.0f);
    const FrustumPlanes f = frustum_from_viewproj(vp);

    EXPECT_FALSE(frustum_culls_sphere(f, Vec3{0, 0, -10.0f}, 1.0f)); // ahead
    EXPECT_TRUE(frustum_culls_sphere(f, Vec3{0, 0, 10.0f}, 1.0f));   // behind
    EXPECT_TRUE(frustum_culls_sphere(f, Vec3{0, 0, -200.0f}, 1.0f)); // past far
    EXPECT_TRUE(frustum_culls_sphere(f, Vec3{50.0f, 0, -10.0f}, 1.0f)); // right
    // Straddling the left plane: kept (conservative).
    EXPECT_FALSE(frustum_culls_sphere(f, Vec3{-10.0f, 0, -10.0f}, 2.0f));
}

TEST(Frustum, OrthoProjectionCullsByRectangle)
{
    const Mat4 vp = mat4_ortho(5.0f, 5.0f, 1.0f, 50.0f);
    const FrustumPlanes f = frustum_from_viewproj(vp);
    EXPECT_FALSE(frustum_culls_sphere(f, Vec3{0, 0, -10.0f}, 1.0f));
    EXPECT_TRUE(frustum_culls_sphere(f, Vec3{8.0f, 0, -10.0f}, 1.0f));
    EXPECT_FALSE(frustum_culls_sphere(f, Vec3{5.5f, 0, -10.0f}, 1.0f)); // straddle
}
