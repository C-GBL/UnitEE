// BVH construction and world queries (M11 tasks 1, 2). The BVH is built by
// the same code the offline baker calls, so what these tests pin is what
// ships.
#include "ps2ur/phys.h"
#include "ps2ur/phys_bvh.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace ps2ur;
using namespace ps2ur::phys;

namespace {

Vec3 v3(float x, float y, float z) { return Vec3{x, y, z}; }

// A flat grid of triangles on y=0, spanning [0,size] on x and z. Enough
// triangles to force a real tree rather than a single leaf.
struct Grid {
    std::vector<Vec3> vertices;
    std::vector<BvhTriangle> triangles;
    std::vector<BvhNode> nodes;
    StaticMesh mesh;

    void build(uint32_t cells, float cell_size, uint8_t layer = 0)
    {
        const uint32_t stride = cells + 1;
        for (uint32_t z = 0; z < stride; ++z) {
            for (uint32_t x = 0; x < stride; ++x) {
                vertices.push_back(v3(static_cast<float>(x) * cell_size, 0.0f,
                                      static_cast<float>(z) * cell_size));
            }
        }
        for (uint32_t z = 0; z < cells; ++z) {
            for (uint32_t x = 0; x < cells; ++x) {
                const uint16_t a = static_cast<uint16_t>(z * stride + x);
                const uint16_t b = static_cast<uint16_t>(a + 1);
                const uint16_t c = static_cast<uint16_t>(a + stride);
                const uint16_t d = static_cast<uint16_t>(c + 1);
                // Wound so the face normal is +Y.
                triangles.push_back(BvhTriangle{a, c, b, layer, 0});
                triangles.push_back(BvhTriangle{b, c, d, layer, 0});
            }
        }
        finish();
    }

    void finish()
    {
        nodes.resize(bvh_max_nodes(static_cast<uint32_t>(triangles.size())));
        const uint32_t written = build_bvh(
            triangles.data(), static_cast<uint32_t>(triangles.size()),
            vertices.data(), static_cast<uint32_t>(vertices.size()),
            nodes.data(), static_cast<uint32_t>(nodes.size()));
        mesh.nodes = nodes.data();
        mesh.node_count = written;
        mesh.triangles = triangles.data();
        mesh.triangle_count = static_cast<uint32_t>(triangles.size());
        mesh.vertices = vertices.data();
        mesh.vertex_count = static_cast<uint32_t>(vertices.size());
    }
};

// Every test owns the module state; physics is a singleton by design (one
// world per console) so the tests reset rather than instantiate.
struct PhysFixture : public ::testing::Test {
    void SetUp() override
    {
        shutdown();
        init();
    }
    void TearDown() override { shutdown(); }
};

} // namespace

// ---- BVH construction ------------------------------------------------------

TEST(PhysBvh, BuildsAValidTreeOverAGrid)
{
    Grid grid;
    grid.build(8, 1.0f); // 128 triangles

    ASSERT_GT(grid.mesh.node_count, 1u) << "128 triangles must not be one leaf";
    const char* error = nullptr;
    EXPECT_TRUE(validate_bvh(grid.mesh, &error)) << error;
}

TEST(PhysBvh, EveryTriangleEndsUpInExactlyOneLeaf)
{
    Grid grid;
    grid.build(6, 2.0f);
    // validate_bvh checks coverage and non-overlap; assert the count too, so
    // a tree that "covers" by putting everything in one giant leaf is still
    // caught by the node count above.
    uint32_t leaf_triangles = 0;
    for (uint32_t i = 0; i < grid.mesh.node_count; ++i) {
        leaf_triangles += grid.mesh.nodes[i].count;
    }
    EXPECT_EQ(leaf_triangles, grid.mesh.triangle_count);
}

TEST(PhysBvh, ChildBoundsStayInsideTheirParent)
{
    Grid grid;
    grid.build(8, 1.0f);
    for (uint32_t i = 0; i < grid.mesh.node_count; ++i) {
        const BvhNode& node = grid.mesh.nodes[i];
        if (node.count != 0) {
            continue;
        }
        ASSERT_LT(node.first + 1u, grid.mesh.node_count);
        for (uint32_t c = 0; c < 2; ++c) {
            const BvhNode& child = grid.mesh.nodes[node.first + c];
            EXPECT_GE(child.bmin.x, node.bmin.x - 1e-3f);
            EXPECT_LE(child.bmax.y, node.bmax.y + 1e-3f);
            EXPECT_LE(child.bmax.z, node.bmax.z + 1e-3f);
        }
    }
}

TEST(PhysBvh, CoincidentTrianglesDoNotRecurseForever)
{
    // Identical centroids never split on any axis. Without the median
    // fallback and the depth cap this recurses until the stack dies.
    Grid grid;
    for (uint32_t i = 0; i < 3; ++i) {
        grid.vertices.push_back(v3(0, 0, 0));
        grid.vertices.push_back(v3(1, 0, 0));
        grid.vertices.push_back(v3(0, 0, 1));
    }
    for (uint16_t i = 0; i < 32; ++i) {
        grid.triangles.push_back(BvhTriangle{0, 1, 2, 0, 0});
    }
    grid.finish();
    EXPECT_GT(grid.mesh.node_count, 0u) << "the build must terminate";
    const char* error = nullptr;
    EXPECT_TRUE(validate_bvh(grid.mesh, &error)) << error;
}

TEST(PhysBvh, AnEmptyMeshBuildsAnEmptyTreeRatherThanFailing)
{
    BvhNode nodes[4];
    const uint32_t written = build_bvh(nullptr, 0, nullptr, 0, nodes, 4);
    EXPECT_EQ(written, 1u) << "an empty tree still needs a root";
    EXPECT_EQ(nodes[0].count, 0u);
}

TEST(PhysBvh, AnOutOfRangeVertexIndexIsRefused)
{
    Vec3 vertices[3] = {v3(0, 0, 0), v3(1, 0, 0), v3(0, 0, 1)};
    BvhTriangle triangles[1] = {BvhTriangle{0, 1, 99, 0, 0}};
    BvhNode nodes[8];
    EXPECT_EQ(build_bvh(triangles, 1, vertices, 3, nodes, 8), 0u)
        << "a bad index must be caught at build time, not dereferenced";
}

TEST(PhysBvh, ValidateRejectsATreeWhoseLeafPointsPastTheTriangles)
{
    Grid grid;
    grid.build(4, 1.0f);
    // Corrupt a leaf the way a bad exporter would.
    for (uint32_t i = 0; i < grid.mesh.node_count; ++i) {
        if (grid.nodes[i].count > 0) {
            grid.nodes[i].first = grid.mesh.triangle_count;
            break;
        }
    }
    const char* error = nullptr;
    EXPECT_FALSE(validate_bvh(grid.mesh, &error));
    EXPECT_STRNE(error, "");
}

// ---- raycast ---------------------------------------------------------------

TEST_F(PhysFixture, RaycastHitsTheGroundBelowAndReportsDistanceAndNormal)
{
    Grid grid;
    grid.build(8, 1.0f);
    set_static_mesh(&grid.mesh);

    RaycastHit hit;
    ASSERT_TRUE(raycast(v3(4, 10, 4), v3(0, -1, 0), 100.0f, kAllLayers, &hit));
    EXPECT_NEAR(hit.distance, 10.0f, 1e-3f);
    EXPECT_NEAR(hit.point.y, 0.0f, 1e-3f);
    EXPECT_GT(hit.normal.y, 0.9f);
    EXPECT_GE(hit.triangle, 0);
    EXPECT_EQ(hit.collider, -1) << "a static hit names no collider";
}

TEST_F(PhysFixture, RaycastMissesBesideTheGeometryAndBeyondItsRange)
{
    Grid grid;
    grid.build(4, 1.0f); // spans 0..4
    set_static_mesh(&grid.mesh);

    RaycastHit hit;
    EXPECT_FALSE(raycast(v3(50, 10, 50), v3(0, -1, 0), 100.0f, kAllLayers, &hit));
    EXPECT_FALSE(raycast(v3(2, 10, 2), v3(0, -1, 0), 5.0f, kAllLayers, &hit))
        << "max_distance must actually bound the query";
}

TEST_F(PhysFixture, RaycastReturnsTheNEARESTHitNotJustAnyHit)
{
    // Two floors: one at y=0 from the grid, one collider sphere above it.
    Grid grid;
    grid.build(4, 1.0f);
    set_static_mesh(&grid.mesh);

    Collider sphere;
    sphere.kind = ColliderKind::Sphere;
    sphere.half_extents = v3(1.0f, 0, 0);
    const int32_t index = add_collider(sphere);
    ASSERT_GE(index, 0);

    Vec3 positions[1] = {v3(2, 5, 2)};
    Quat rotations[1] = {quat_identity()};
    step(positions, rotations, 1);

    RaycastHit hit;
    ASSERT_TRUE(raycast(v3(2, 20, 2), v3(0, -1, 0), 100.0f, kAllLayers, &hit));
    EXPECT_EQ(hit.collider, index)
        << "the sphere at y=5 is nearer than the ground at y=0";
    EXPECT_NEAR(hit.distance, 14.0f, 1e-2f);
}

TEST_F(PhysFixture, RaycastHitsEachPrimitiveKind)
{
    Collider box;
    box.kind = ColliderKind::Box;
    box.half_extents = v3(1, 1, 1);
    const int32_t bi = add_collider(box);

    Collider capsule;
    capsule.kind = ColliderKind::Capsule;
    capsule.half_extents = v3(0.5f, 0, 0);
    capsule.height = 2.0f;
    const int32_t ci = add_collider(capsule);

    Vec3 positions[2] = {v3(0, 0, 0), v3(10, 0, 0)};
    Quat rotations[2] = {quat_identity(), quat_identity()};
    step(positions, rotations, 2);

    RaycastHit hit;
    ASSERT_TRUE(raycast(v3(0, 10, 0), v3(0, -1, 0), 100.0f, kAllLayers, &hit));
    EXPECT_EQ(hit.collider, bi);
    EXPECT_NEAR(hit.distance, 9.0f, 1e-3f);

    ASSERT_TRUE(raycast(v3(10, 10, 0), v3(0, -1, 0), 100.0f, kAllLayers, &hit));
    EXPECT_EQ(hit.collider, ci);
    EXPECT_NEAR(hit.distance, 9.0f, 1e-2f);
}

TEST_F(PhysFixture, ARayWithNoDirectionIsRefusedRatherThanDividingByZero)
{
    RaycastHit hit;
    EXPECT_FALSE(raycast(v3(0, 0, 0), v3(0, 0, 0), 10.0f, kAllLayers, &hit));
    EXPECT_FALSE(raycast(v3(0, 0, 0), v3(0, -1, 0), 0.0f, kAllLayers, &hit));
}

// ---- layer masks -----------------------------------------------------------

TEST_F(PhysFixture, ALayerMaskFiltersStaticTrianglesAndColliders)
{
    Grid grid;
    grid.build(4, 1.0f, /*layer=*/3);
    set_static_mesh(&grid.mesh);

    RaycastHit hit;
    EXPECT_TRUE(raycast(v3(2, 10, 2), v3(0, -1, 0), 100.0f, 1u << 3, &hit));
    EXPECT_FALSE(raycast(v3(2, 10, 2), v3(0, -1, 0), 100.0f, 1u << 4, &hit))
        << "a mask that excludes the triangle's layer must miss it";
    EXPECT_EQ(hit.layer, 3);
}

TEST_F(PhysFixture, TheLayerMatrixIsSymmetricAndDefaultsToEverythingColliding)
{
    EXPECT_TRUE(layers_collide(0, 5));
    set_layers_collide(2, 7, false);
    EXPECT_FALSE(layers_collide(2, 7));
    EXPECT_FALSE(layers_collide(7, 2))
        << "a one-way collision rule is not a thing Unity can express";
    set_layers_collide(7, 2, true);
    EXPECT_TRUE(layers_collide(2, 7));

    reset_layer_matrix(false);
    EXPECT_FALSE(layers_collide(0, 0));
    EXPECT_EQ(layer_mask(0), 0u);
}

TEST_F(PhysFixture, AnOutOfRangeLayerIsIgnoredRatherThanCorruptingTheMatrix)
{
    set_layers_collide(99, 0, false);
    EXPECT_TRUE(layers_collide(0, 1)) << "the matrix must be untouched";
    EXPECT_FALSE(layers_collide(99, 0));
    EXPECT_EQ(layer_mask(99), 0u);
}

// ---- sweeps ----------------------------------------------------------------

TEST_F(PhysFixture, SpherecastStopsAtTheSurfaceNotAtTheCentre)
{
    Grid grid;
    grid.build(8, 1.0f);
    set_static_mesh(&grid.mesh);

    RaycastHit hit;
    ASSERT_TRUE(spherecast(v3(4, 10, 4), 1.0f, v3(0, -1, 0), 100.0f, kAllLayers,
                           &hit));
    // A 1 m sphere dropped from y=10 touches y=0 after 9 m, not 10.
    EXPECT_NEAR(hit.distance, 9.0f, 0.2f);
}

TEST_F(PhysFixture, ASweepThatStartsAlreadyOverlappingReportsZeroFraction)
{
    Grid grid;
    grid.build(4, 1.0f);
    set_static_mesh(&grid.mesh);

    CapsuleShape capsule;
    capsule.a = v3(2, 0, 2); // straddling the ground plane
    capsule.b = v3(2, 1, 2);
    capsule.radius = 0.5f;

    float fraction = 1.0f;
    Vec3 normal;
    int32_t triangle = -1;
    ASSERT_TRUE(sweep_capsule(capsule, v3(1, 0, 0), kAllLayers, &fraction,
                              &normal, &triangle));
    EXPECT_FLOAT_EQ(fraction, 0.0f)
        << "an already-overlapping sweep must depenetrate, not slide";
}

TEST_F(PhysFixture, AFastSweepDoesNotTunnelThroughTheFloor)
{
    // The anti-tunnelling guarantee: a discrete test at the end position
    // would find the capsule far below the ground and report no hit at all.
    Grid grid;
    grid.build(8, 1.0f);
    set_static_mesh(&grid.mesh);

    CapsuleShape capsule;
    capsule.a = v3(4, 50, 4);
    capsule.b = v3(4, 51, 4);
    capsule.radius = 0.5f;

    float fraction = 1.0f;
    Vec3 normal;
    int32_t triangle = -1;
    ASSERT_TRUE(sweep_capsule(capsule, v3(0, -100, 0), kAllLayers, &fraction,
                              &normal, &triangle))
        << "a 100 m/step drop must still find the floor it passes through";
    EXPECT_GT(fraction, 0.0f);
    EXPECT_LT(fraction, 1.0f);
    // Stopped at the surface: 50 down minus the radius, over 100 travelled.
    EXPECT_NEAR(fraction, 0.495f, 0.02f);
}

// ---- overlap ---------------------------------------------------------------

TEST_F(PhysFixture, OverlapSphereFindsCollidersAndRespectsTheMask)
{
    Collider a;
    a.kind = ColliderKind::Sphere;
    a.half_extents = v3(1, 0, 0);
    a.layer = 1;
    Collider b = a;
    b.layer = 2;
    const int32_t ia = add_collider(a);
    const int32_t ib = add_collider(b);

    Vec3 positions[2] = {v3(0, 0, 0), v3(1, 0, 0)};
    Quat rotations[2] = {quat_identity(), quat_identity()};
    step(positions, rotations, 2);

    int32_t found[4];
    EXPECT_EQ(overlap_sphere(v3(0.5f, 0, 0), 0.5f, kAllLayers, found, 4), 2u);
    EXPECT_EQ(overlap_sphere(v3(0.5f, 0, 0), 0.5f, 1u << 1, found, 4), 1u);
    EXPECT_EQ(found[0], ia);
    EXPECT_EQ(overlap_sphere(v3(0.5f, 0, 0), 0.5f, 1u << 2, found, 4), 1u);
    EXPECT_EQ(found[0], ib);
    EXPECT_EQ(overlap_sphere(v3(100, 0, 0), 1.0f, kAllLayers, found, 4), 0u);
}

TEST_F(PhysFixture, OverlapReportsTheTrueCountEvenWhenItCannotWriteThemAll)
{
    for (uint32_t i = 0; i < 5; ++i) {
        Collider c;
        c.kind = ColliderKind::Sphere;
        c.half_extents = v3(1, 0, 0);
        add_collider(c);
    }
    Vec3 positions[5] = {};
    Quat rotations[5] = {quat_identity(), quat_identity(), quat_identity(),
                         quat_identity(), quat_identity()};
    step(positions, rotations, 5);

    int32_t found[2];
    EXPECT_EQ(overlap_sphere(v3(0, 0, 0), 1.0f, kAllLayers, found, 2), 5u)
        << "a truncated result must still say how many there really were";
}

// ---- statistics ------------------------------------------------------------

TEST_F(PhysFixture, TheBvhActuallyReducesTriangleTests)
{
    // The point of a tree: a query near one corner must not test the whole
    // mesh. 512 triangles, and a ray at one corner should touch a handful.
    Grid grid;
    grid.build(16, 1.0f);
    set_static_mesh(&grid.mesh);
    ASSERT_EQ(grid.mesh.triangle_count, 512u);

    reset_stats();
    RaycastHit hit;
    ASSERT_TRUE(raycast(v3(0.5f, 5, 0.5f), v3(0, -1, 0), 100.0f, kAllLayers,
                        &hit));
    EXPECT_LT(stats().triangle_tests, 64u)
        << "a BVH that tests most of the mesh is not doing its job";
}
