// Intersection primitives (M11 task 2). These are the functions every query
// is assembled from, so they are pinned exactly rather than through the
// behaviour of the world above them.
//
// The contract under test throughout: an out_normal points from the SECOND
// shape towards the FIRST, so moving A along +normal by depth separates the
// pair. A test that only checked "they overlap" would pass with the normal
// backwards, which is the bug that makes objects suck together.
#include "ps2ur/phys.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace ps2ur;
using namespace ps2ur::phys;

namespace {

Vec3 v3(float x, float y, float z) { return Vec3{x, y, z}; }

void expect_vec_near(Vec3 actual, Vec3 expected, float tol = 1e-4f)
{
    EXPECT_NEAR(actual.x, expected.x, tol);
    EXPECT_NEAR(actual.y, expected.y, tol);
    EXPECT_NEAR(actual.z, expected.z, tol);
}

} // namespace

// ---- ray vs triangle -------------------------------------------------------

TEST(PhysRay, HitsATriangleItPointsAtAndReportsTheDistance)
{
    const Vec3 v0 = v3(-1, 0, 0), v1 = v3(1, 0, 0), v2 = v3(0, 0, 2);
    float t = 0.0f;
    Vec3 n;
    ASSERT_TRUE(ray_triangle(v3(0, 5, 0.5f), v3(0, -1, 0), v0, v1, v2, false,
                             &t, &n));
    EXPECT_NEAR(t, 5.0f, 1e-4f);
    // The normal must oppose the ray, whatever the winding happened to be.
    EXPECT_GT(n.y, 0.9f);
}

TEST(PhysRay, MissesOutsideTheTriangleAndBehindTheOrigin)
{
    const Vec3 v0 = v3(-1, 0, 0), v1 = v3(1, 0, 0), v2 = v3(0, 0, 2);
    float t = 0.0f;
    EXPECT_FALSE(ray_triangle(v3(5, 5, 0.5f), v3(0, -1, 0), v0, v1, v2, false,
                              &t, nullptr))
        << "a ray outside the triangle's extent must miss";
    EXPECT_FALSE(ray_triangle(v3(0, 5, 0.5f), v3(0, 1, 0), v0, v1, v2, false,
                              &t, nullptr))
        << "a triangle behind the origin is not a hit";
}

TEST(PhysRay, CullingMakesTheTriangleOneSidedAndTwoSidedDoesNot)
{
    const Vec3 v0 = v3(-1, 0, -1), v1 = v3(1, 0, -1), v2 = v3(0, 0, 1);
    float t = 0.0f;

    // Asserted as a PROPERTY rather than against a hard-coded winding: two
    // sided hits from both directions, culled hits from exactly one. Which
    // side is the front depends on the vertex order, and a test that
    // hard-codes it is testing the test.
    const bool above_two_sided =
        ray_triangle(v3(0, 5, 0), v3(0, -1, 0), v0, v1, v2, false, &t, nullptr);
    const bool below_two_sided =
        ray_triangle(v3(0, -5, 0), v3(0, 1, 0), v0, v1, v2, false, &t, nullptr);
    EXPECT_TRUE(above_two_sided) << "the world queries rely on two-sided hits";
    EXPECT_TRUE(below_two_sided);

    const bool above_culled =
        ray_triangle(v3(0, 5, 0), v3(0, -1, 0), v0, v1, v2, true, &t, nullptr);
    const bool below_culled =
        ray_triangle(v3(0, -5, 0), v3(0, 1, 0), v0, v1, v2, true, &t, nullptr);
    EXPECT_NE(above_culled, below_culled)
        << "culling must accept exactly one side";
}

TEST(PhysRay, AabbSlabTestAcceptsAHitAndRejectsAMiss)
{
    const Vec3 bmin = v3(-1, -1, -1), bmax = v3(1, 1, 1);
    const Vec3 inv_dir = v3(1e30f, -1.0f, 1e30f); // straight down
    float t = 0.0f;
    ASSERT_TRUE(ray_aabb(v3(0, 5, 0), inv_dir, bmin, bmax, 100.0f, &t));
    EXPECT_NEAR(t, 4.0f, 1e-3f);
    EXPECT_FALSE(ray_aabb(v3(5, 5, 5), inv_dir, bmin, bmax, 100.0f, &t));
    // Beyond max_t is a miss even though the geometry is on the line.
    EXPECT_FALSE(ray_aabb(v3(0, 5, 0), inv_dir, bmin, bmax, 1.0f, &t));
}

TEST(PhysRay, AnOriginInsideTheBoxHitsAtZero)
{
    float t = -1.0f;
    ASSERT_TRUE(ray_aabb(v3(0, 0, 0), v3(1e30f, -1.0f, 1e30f), v3(-1, -1, -1),
                         v3(1, 1, 1), 100.0f, &t));
    EXPECT_FLOAT_EQ(t, 0.0f) << "a ray starting inside must not report a "
                                "negative distance";
}

// ---- closest points --------------------------------------------------------

TEST(PhysClosest, PointOnSegmentClampsToTheEndpoints)
{
    expect_vec_near(closest_point_on_segment(v3(0, 0, 0), v3(10, 0, 0), v3(5, 3, 0)),
                    v3(5, 0, 0));
    expect_vec_near(closest_point_on_segment(v3(0, 0, 0), v3(10, 0, 0), v3(-5, 0, 0)),
                    v3(0, 0, 0));
    expect_vec_near(closest_point_on_segment(v3(0, 0, 0), v3(10, 0, 0), v3(50, 0, 0)),
                    v3(10, 0, 0));
}

TEST(PhysClosest, PointOnTriangleCoversFaceEdgeAndVertexRegions)
{
    const Vec3 v0 = v3(0, 0, 0), v1 = v3(4, 0, 0), v2 = v3(0, 0, 4);
    // Above the face.
    expect_vec_near(closest_point_on_triangle(v3(1, 5, 1), v0, v1, v2), v3(1, 0, 1));
    // Beyond an edge.
    expect_vec_near(closest_point_on_triangle(v3(5, 0, -1), v0, v1, v2), v3(4, 0, 0));
    // Beyond a vertex.
    expect_vec_near(closest_point_on_triangle(v3(-3, 0, -3), v0, v1, v2), v3(0, 0, 0));
    // On the hypotenuse side.
    const Vec3 edge = closest_point_on_triangle(v3(4, 0, 4), v0, v1, v2);
    EXPECT_NEAR(edge.x + edge.z, 4.0f, 1e-3f)
        << "the closest point should land on the x+z=4 edge";
}

TEST(PhysClosest, SegmentPairHandlesSkewAndParallelCases)
{
    Vec3 c1, c2;
    // Skew: one along x at y=0, one along z at y=2.
    closest_points_segments(v3(-5, 0, 0), v3(5, 0, 0), v3(0, 2, -5), v3(0, 2, 5),
                            &c1, &c2);
    expect_vec_near(c1, v3(0, 0, 0));
    expect_vec_near(c2, v3(0, 2, 0));

    // Parallel: the general solution divides by zero here, so this is the
    // case that catches a missing degenerate branch.
    closest_points_segments(v3(0, 0, 0), v3(10, 0, 0), v3(0, 3, 0), v3(10, 3, 0),
                            &c1, &c2);
    EXPECT_NEAR(c1.y, 0.0f, 1e-4f);
    EXPECT_NEAR(c2.y, 3.0f, 1e-4f);
    EXPECT_NEAR(length(sub(c1, c2)), 3.0f, 1e-4f)
        << "parallel segments are 3 apart however the parameters land";
}

// ---- overlaps --------------------------------------------------------------

TEST(PhysOverlap, SphereVsSphereReportsDepthAndAPushApartNormal)
{
    Vec3 n;
    float depth = 0.0f;
    ASSERT_TRUE(sphere_sphere(v3(0, 0, 0), 1.0f, v3(1.5f, 0, 0), 1.0f, &n, &depth));
    EXPECT_NEAR(depth, 0.5f, 1e-4f);
    // A is at -x relative to B, so the normal must point -x: moving A along
    // it separates them.
    expect_vec_near(n, v3(-1, 0, 0));
    EXPECT_FALSE(sphere_sphere(v3(0, 0, 0), 1.0f, v3(3, 0, 0), 1.0f, &n, &depth));
}

TEST(PhysOverlap, SphereVsBoxPushesOutOfTheNearestFace)
{
    BoxShape box;
    box.center = v3(0, 0, 0);
    box.half_extents = v3(1, 1, 1);
    box.rotation = quat_identity();

    Vec3 n;
    float depth = 0.0f;
    ASSERT_TRUE(sphere_box(v3(0, 1.5f, 0), 1.0f, box, &n, &depth));
    EXPECT_NEAR(depth, 0.5f, 1e-4f);
    expect_vec_near(n, v3(0, 1, 0));
    EXPECT_FALSE(sphere_box(v3(0, 5, 0), 1.0f, box, &n, &depth));
}

TEST(PhysOverlap, ASphereCentredInsideABoxLeavesByTheShortestRoute)
{
    BoxShape box;
    box.center = v3(0, 0, 0);
    box.half_extents = v3(4, 1, 4); // a slab: up is much the nearest way out
    box.rotation = quat_identity();

    Vec3 n;
    float depth = 0.0f;
    ASSERT_TRUE(sphere_box(v3(0, 0.5f, 0), 0.25f, box, &n, &depth));
    // A centre inside must exit through the least-penetrated axis.
    expect_vec_near(n, v3(0, 1, 0));
    EXPECT_GT(depth, 0.0f);
}

TEST(PhysOverlap, ARotatedBoxIsTestedInItsOwnFrame)
{
    BoxShape box;
    box.center = v3(0, 0, 0);
    box.half_extents = v3(2, 0.5f, 0.5f);
    // 90 degrees about Y sends the long axis from x to -z.
    box.rotation = quat_from_axis_angle(v3(0, 1, 0), 1.5707963f);

    Vec3 n;
    float depth = 0.0f;
    // Along the box's ORIGINAL long axis there is now nothing at 1.8.
    EXPECT_FALSE(sphere_box(v3(1.8f, 0, 0), 0.2f, box, &n, &depth));
    // Along z, where the long axis now points, there is.
    EXPECT_TRUE(sphere_box(v3(0, 0, 1.8f), 0.4f, box, &n, &depth));
}

TEST(PhysOverlap, CapsuleVsCapsuleUsesTheClosestPointsOnTheAxes)
{
    CapsuleShape a;
    a.a = v3(-5, 0, 0);
    a.b = v3(5, 0, 0);
    a.radius = 0.5f;
    CapsuleShape b;
    b.a = v3(0, 0.8f, -5);
    b.b = v3(0, 0.8f, 5);
    b.radius = 0.5f;

    Vec3 n;
    float depth = 0.0f;
    ASSERT_TRUE(capsule_capsule(a, b, &n, &depth));
    EXPECT_NEAR(depth, 0.2f, 1e-4f);
    expect_vec_near(n, v3(0, -1, 0)); // A is below B

    b.a = v3(0, 5, -5);
    b.b = v3(0, 5, 5);
    EXPECT_FALSE(capsule_capsule(a, b, &n, &depth));
}

TEST(PhysOverlap, SphereVsTriangleFallsBackToTheFaceNormalOnTheSurface)
{
    const Vec3 v0 = v3(-5, 0, -5), v1 = v3(5, 0, -5), v2 = v3(0, 0, 5);
    Vec3 n;
    float depth = 0.0f;
    // Centre exactly on the face: there is no direction from the closest
    // point, so the face normal is the only correct answer.
    ASSERT_TRUE(sphere_triangle(v3(0, 0, 0), 1.0f, v0, v1, v2, &n, &depth));
    EXPECT_NEAR(fabsf(n.y), 1.0f, 1e-4f);
    EXPECT_NEAR(depth, 1.0f, 1e-4f);
}

TEST(PhysOverlap, CapsuleVsTriangleTouchesThroughTheEndCap)
{
    const Vec3 v0 = v3(-5, 0, -5), v1 = v3(5, 0, -5), v2 = v3(0, 0, 5);
    CapsuleShape capsule;
    capsule.a = v3(0, 0.4f, 0); // lower cap centre 0.4 above the plane
    capsule.b = v3(0, 3.0f, 0);
    capsule.radius = 0.5f;

    Vec3 n;
    float depth = 0.0f;
    ASSERT_TRUE(capsule_triangle(capsule, v0, v1, v2, &n, &depth));
    EXPECT_NEAR(depth, 0.1f, 1e-3f);
    EXPECT_GT(n.y, 0.9f);

    capsule.a = v3(0, 2.0f, 0);
    capsule.b = v3(0, 5.0f, 0);
    EXPECT_FALSE(capsule_triangle(capsule, v0, v1, v2, &n, &depth));
}

TEST(PhysOverlap, BoxVsBoxFindsTheMinimumSeparatingAxis)
{
    BoxShape a;
    a.center = v3(0, 0, 0);
    a.half_extents = v3(1, 1, 1);
    a.rotation = quat_identity();
    BoxShape b;
    b.center = v3(0, 1.5f, 0);
    b.half_extents = v3(1, 1, 1);
    b.rotation = quat_identity();

    Vec3 n;
    float depth = 0.0f;
    ASSERT_TRUE(box_box(a, b, &n, &depth));
    EXPECT_NEAR(depth, 0.5f, 1e-4f);
    // A is below B, so it is pushed down.
    expect_vec_near(n, v3(0, -1, 0));

    b.center = v3(0, 5, 0);
    EXPECT_FALSE(box_box(a, b, &n, &depth));
}

TEST(PhysOverlap, BoxVsBoxSeparatesOnAnEdgeEdgeAxisAParallelTestWouldMiss)
{
    // Two boxes rotated 45 degrees about different axes only separate on an
    // edge-edge cross product; a 6-axis test would call this a hit.
    BoxShape a;
    a.center = v3(0, 0, 0);
    a.half_extents = v3(1, 1, 1);
    a.rotation = quat_from_axis_angle(v3(0, 0, 1), 0.7853981f);
    BoxShape b;
    b.center = v3(2.6f, 0, 0);
    b.half_extents = v3(1, 1, 1);
    b.rotation = quat_from_axis_angle(v3(1, 0, 0), 0.7853981f);

    Vec3 n;
    float depth = 0.0f;
    // A 45-degree box has a half-width of sqrt(2) ~ 1.414 along x, so the two
    // just clear at 2.6 apart.
    EXPECT_FALSE(box_box(a, b, &n, &depth));
    b.center = v3(2.2f, 0, 0);
    EXPECT_TRUE(box_box(a, b, &n, &depth));
}

// ---- collider -> shape -----------------------------------------------------

TEST(PhysShapes, CapsuleHeightIncludesTheCapsLikeUnity)
{
    Collider c;
    c.kind = ColliderKind::Capsule;
    c.axis = CapsuleAxis::Y;
    c.half_extents = v3(0.5f, 0, 0); // radius
    c.height = 2.0f;                 // total, so the segment is 1.0 long

    const CapsuleShape shape = capsule_shape(c, v3(0, 0, 0), quat_identity());
    EXPECT_NEAR(shape.radius, 0.5f, 1e-5f);
    EXPECT_NEAR(shape.a.y, -0.5f, 1e-5f);
    EXPECT_NEAR(shape.b.y, 0.5f, 1e-5f);
    EXPECT_NEAR(length(sub(shape.b, shape.a)) + 2.0f * shape.radius, 2.0f, 1e-5f)
        << "segment + two caps must equal the stated height";
}

TEST(PhysShapes, ACapsuleShorterThanItsDiameterBecomesASphereNotAnInvertedOne)
{
    Collider c;
    c.kind = ColliderKind::Capsule;
    c.half_extents = v3(1.0f, 0, 0);
    c.height = 0.5f; // less than one diameter

    const CapsuleShape shape = capsule_shape(c, v3(0, 0, 0), quat_identity());
    EXPECT_NEAR(length(sub(shape.b, shape.a)), 0.0f, 1e-5f)
        << "the segment must clamp to zero rather than go negative";
}

TEST(PhysShapes, TheCapsuleAxisFollowsTheColliderDirection)
{
    Collider c;
    c.kind = ColliderKind::Capsule;
    c.half_extents = v3(0.5f, 0, 0);
    c.height = 3.0f;

    c.axis = CapsuleAxis::X;
    CapsuleShape shape = capsule_shape(c, v3(0, 0, 0), quat_identity());
    EXPECT_NEAR(shape.b.x - shape.a.x, 2.0f, 1e-4f);
    c.axis = CapsuleAxis::Z;
    shape = capsule_shape(c, v3(0, 0, 0), quat_identity());
    EXPECT_NEAR(shape.b.z - shape.a.z, 2.0f, 1e-4f);
}

TEST(PhysShapes, TheEntityRotationOrientsAndTheCentreOffsets)
{
    Collider c;
    c.kind = ColliderKind::Capsule;
    c.half_extents = v3(0.5f, 0, 0);
    c.height = 3.0f;
    c.center = v3(0, 0, 2);

    // 90 degrees about Z sends +y to -x, so an upright capsule lies along x.
    const Quat rot = quat_from_axis_angle(v3(0, 0, 1), 1.5707963f);
    const CapsuleShape shape = capsule_shape(c, v3(10, 0, 0), rot);
    EXPECT_NEAR(fabsf(shape.b.x - shape.a.x), 2.0f, 1e-3f);
    // The local +z offset is unaffected by a rotation about z.
    const Vec3 mid = scale(add(shape.a, shape.b), 0.5f);
    expect_vec_near(mid, v3(10, 0, 2), 1e-3f);
}
