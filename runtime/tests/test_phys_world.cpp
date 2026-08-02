// The stepped world: the rigid body integrator, contact events, the
// character controller and determinism (M11 tasks 2, 3, 5).
//
// These tests assert BEHAVIOUR over many steps rather than single-call
// outputs, because that is where an integrator goes wrong: explicit Euler
// looks fine for one step and gains energy over a hundred.
#include "ps2ur/phys.h"
#include "ps2ur/phys_bvh.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace ps2ur;
using namespace ps2ur::phys;

namespace {

Vec3 v3(float x, float y, float z) { return Vec3{x, y, z}; }

// A flat floor on y=0 spanning [-half, half] on x and z, as two triangles
// per cell so the BVH has something to partition.
struct Floor {
    std::vector<Vec3> vertices;
    std::vector<BvhTriangle> triangles;
    std::vector<BvhNode> nodes;
    StaticMesh mesh;

    void build(uint32_t cells, float cell_size, float y = 0.0f)
    {
        const uint32_t stride = cells + 1;
        const float origin = -0.5f * static_cast<float>(cells) * cell_size;
        for (uint32_t z = 0; z < stride; ++z) {
            for (uint32_t x = 0; x < stride; ++x) {
                vertices.push_back(v3(origin + static_cast<float>(x) * cell_size,
                                      y,
                                      origin + static_cast<float>(z) * cell_size));
            }
        }
        for (uint32_t z = 0; z < cells; ++z) {
            for (uint32_t x = 0; x < cells; ++x) {
                const uint16_t a = static_cast<uint16_t>(z * stride + x);
                const uint16_t b = static_cast<uint16_t>(a + 1);
                const uint16_t c = static_cast<uint16_t>(a + stride);
                const uint16_t d = static_cast<uint16_t>(c + 1);
                triangles.push_back(BvhTriangle{a, c, b, 0, 0});
                triangles.push_back(BvhTriangle{b, c, d, 0, 0});
            }
        }
        finish();
    }

    // A vertical wall in the x=at plane, spanning z and y.
    void add_wall(float at, float extent, float height)
    {
        const uint16_t base = static_cast<uint16_t>(vertices.size());
        vertices.push_back(v3(at, 0, -extent));
        vertices.push_back(v3(at, 0, extent));
        vertices.push_back(v3(at, height, -extent));
        vertices.push_back(v3(at, height, extent));
        triangles.push_back(BvhTriangle{base, static_cast<uint16_t>(base + 1),
                                        static_cast<uint16_t>(base + 2), 0, 0});
        triangles.push_back(BvhTriangle{static_cast<uint16_t>(base + 1),
                                        static_cast<uint16_t>(base + 3),
                                        static_cast<uint16_t>(base + 2), 0, 0});
    }

    // A horizontal ledge at 'height', starting at x = at and running +x.
    void add_ledge(float at, float extent, float height, float depth)
    {
        const uint16_t base = static_cast<uint16_t>(vertices.size());
        vertices.push_back(v3(at, height, -extent));
        vertices.push_back(v3(at, height, extent));
        vertices.push_back(v3(at + depth, height, -extent));
        vertices.push_back(v3(at + depth, height, extent));
        triangles.push_back(BvhTriangle{base, static_cast<uint16_t>(base + 2),
                                        static_cast<uint16_t>(base + 1), 0, 0});
        triangles.push_back(BvhTriangle{static_cast<uint16_t>(base + 1),
                                        static_cast<uint16_t>(base + 2),
                                        static_cast<uint16_t>(base + 3), 0, 0});
    }

    // A ramp rising along +x at the given angle.
    void add_ramp(float degrees, float extent, float length)
    {
        const float rise = length * tanf(degrees * 3.14159265f / 180.0f);
        const uint16_t base = static_cast<uint16_t>(vertices.size());
        vertices.push_back(v3(0, 0, -extent));
        vertices.push_back(v3(0, 0, extent));
        vertices.push_back(v3(length, rise, -extent));
        vertices.push_back(v3(length, rise, extent));
        triangles.push_back(BvhTriangle{base, static_cast<uint16_t>(base + 2),
                                        static_cast<uint16_t>(base + 1), 0, 0});
        triangles.push_back(BvhTriangle{static_cast<uint16_t>(base + 1),
                                        static_cast<uint16_t>(base + 2),
                                        static_cast<uint16_t>(base + 3), 0, 0});
    }

    void finish()
    {
        nodes.assign(bvh_max_nodes(static_cast<uint32_t>(triangles.size())),
                     BvhNode{});
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

struct PhysWorld : public ::testing::Test {
    void SetUp() override
    {
        shutdown();
        init();
    }
    void TearDown() override { shutdown(); }
};

} // namespace

// ---- integrator ------------------------------------------------------------

TEST_F(PhysWorld, AFallingBodyFollowsTheAnalyticSolutionCloselyEnough)
{
    Collider c;
    c.kind = ColliderKind::Sphere;
    c.half_extents = v3(0.5f, 0, 0);
    RigidBody b;
    b.mass = 1.0f;
    const int32_t bi = add_body(b);
    c.body = bi;
    add_collider(c);

    Vec3 positions[1] = {v3(0, 100, 0)};
    Quat rotations[1] = {quat_identity()};

    const float dt = fixed_timestep();
    const uint32_t steps = 30; // one second
    for (uint32_t i = 0; i < steps; ++i) {
        step(positions, rotations, 1);
    }
    const float elapsed = dt * static_cast<float>(steps);
    // Semi-implicit Euler over-integrates position by exactly g*dt^2/2 per
    // step relative to the closed form, so the drop is a little further than
    // 0.5*g*t^2. Asserting the closed form to a loose tolerance is the point:
    // the discretisation error must stay small and bounded, not vanish.
    const float analytic = 100.0f - 0.5f * 9.81f * elapsed * elapsed;
    EXPECT_NEAR(positions[0].y, analytic, 0.25f);
    EXPECT_NEAR(body(0).velocity.y, -9.81f * elapsed, 0.35f);
}

TEST_F(PhysWorld, SemiImplicitEulerDoesNotGainEnergyOverAThousandSteps)
{
    // The property that makes symplectic integration worth choosing: a
    // frictionless body bouncing on a floor must not climb. Explicit Euler
    // gains energy every step and this test would fail by metres.
    Floor floor;
    floor.build(8, 4.0f);
    set_static_mesh(&floor.mesh);

    Collider c;
    c.kind = ColliderKind::Sphere;
    c.half_extents = v3(0.5f, 0, 0);
    RigidBody b;
    b.mass = 1.0f;
    b.restitution = 1.0f; // perfectly elastic: nothing should damp it
    c.body = add_body(b);
    add_collider(c);

    Vec3 positions[1] = {v3(0, 5, 0)};
    Quat rotations[1] = {quat_identity()};

    float highest = 0.0f;
    for (uint32_t i = 0; i < 1000; ++i) {
        step(positions, rotations, 1);
        if (positions[0].y > highest) {
            highest = positions[0].y;
        }
    }
    // Started at 5; it may lose height, but it must never end up higher.
    EXPECT_LE(highest, 5.5f)
        << "the integrator is injecting energy: peak height grew to " << highest;
}

TEST_F(PhysWorld, AKinematicBodyIgnoresGravityAndTheSolver)
{
    Collider c;
    c.kind = ColliderKind::Sphere;
    c.half_extents = v3(0.5f, 0, 0);
    RigidBody b;
    b.is_kinematic = true;
    c.body = add_body(b);
    add_collider(c);

    Vec3 positions[1] = {v3(0, 10, 0)};
    Quat rotations[1] = {quat_identity()};
    for (uint32_t i = 0; i < 30; ++i) {
        step(positions, rotations, 1);
    }
    EXPECT_FLOAT_EQ(positions[0].y, 10.0f)
        << "gameplay owns a kinematic body's position, not the solver";
}

TEST_F(PhysWorld, ABodyComesToRestOnTheFloorAndSleeps)
{
    Floor floor;
    floor.build(4, 4.0f);
    set_static_mesh(&floor.mesh);

    Collider c;
    c.kind = ColliderKind::Sphere;
    c.half_extents = v3(0.5f, 0, 0);
    RigidBody b;
    b.restitution = 0.0f;
    c.body = add_body(b);
    add_collider(c);

    Vec3 positions[1] = {v3(0, 3, 0)};
    Quat rotations[1] = {quat_identity()};
    for (uint32_t i = 0; i < 200; ++i) {
        step(positions, rotations, 1);
    }
    // Resting on the floor means the centre sits about one radius up.
    EXPECT_NEAR(positions[0].y, 0.5f, 0.1f);
    EXPECT_TRUE(body(0).sleeping)
        << "a body at rest must sleep or it costs solver time forever";
}

TEST_F(PhysWorld, DragSlowsABodyWithoutEverReversingIt)
{
    Collider c;
    c.kind = ColliderKind::Sphere;
    c.half_extents = v3(0.5f, 0, 0);
    RigidBody b;
    b.use_gravity = false;
    b.drag = 20.0f; // absurd on purpose: a subtracted force would flip sign
    b.velocity = v3(10, 0, 0);
    c.body = add_body(b);
    add_collider(c);

    Vec3 positions[1] = {v3(0, 0, 0)};
    Quat rotations[1] = {quat_identity()};
    for (uint32_t i = 0; i < 60; ++i) {
        step(positions, rotations, 1);
        EXPECT_GE(body(0).velocity.x, 0.0f)
            << "drag must decay velocity, never reverse it";
    }
    EXPECT_LT(body(0).velocity.x, 1.0f);
}

// ---- static resolution -----------------------------------------------------

TEST_F(PhysWorld, ABodyDoesNotSinkThroughTheFloorItLandsOn)
{
    Floor floor;
    floor.build(8, 4.0f);
    set_static_mesh(&floor.mesh);

    Collider c;
    c.kind = ColliderKind::Sphere;
    c.half_extents = v3(0.5f, 0, 0);
    c.body = add_body(RigidBody{});
    add_collider(c);

    Vec3 positions[1] = {v3(0, 8, 0)};
    Quat rotations[1] = {quat_identity()};
    for (uint32_t i = 0; i < 300; ++i) {
        step(positions, rotations, 1);
        EXPECT_GT(positions[0].y, -0.5f)
            << "sank through the floor at step " << i;
    }
}

// ---- contact events (M11 task 3) -------------------------------------------

TEST_F(PhysWorld, TriggerContactsGoEnterThenStayThenExitInThatOrder)
{
    Collider trigger;
    trigger.kind = ColliderKind::Sphere;
    trigger.half_extents = v3(1.0f, 0, 0);
    trigger.is_trigger = true;
    const int32_t ti = add_collider(trigger);

    Collider mover;
    mover.kind = ColliderKind::Sphere;
    mover.half_extents = v3(0.5f, 0, 0);
    RigidBody b;
    b.use_gravity = false;
    b.is_kinematic = true; // gameplay drives it; we want a body so pairs run
    mover.body = add_body(b);
    const int32_t mi = add_collider(mover);
    ASSERT_GE(mi, 0);

    Vec3 positions[2] = {v3(0, 0, 0), v3(5, 0, 0)};
    Quat rotations[2] = {quat_identity(), quat_identity()};

    // Far away: no contact at all.
    step(positions, rotations, 2);
    EXPECT_EQ(contact_count(), 0u);

    // Overlapping: Enter.
    positions[1] = v3(1.0f, 0, 0);
    step(positions, rotations, 2);
    ASSERT_EQ(contact_count(), 1u);
    EXPECT_EQ(contact(0).phase, ContactPhase::Enter);
    EXPECT_TRUE(contact(0).is_trigger);
    EXPECT_EQ(contact(0).collider_a, ti < mi ? ti : mi);

    // Still overlapping: Stay, not a second Enter.
    step(positions, rotations, 2);
    ASSERT_EQ(contact_count(), 1u);
    EXPECT_EQ(contact(0).phase, ContactPhase::Stay);

    // Moved away: exactly one Exit, then nothing.
    positions[1] = v3(5, 0, 0);
    step(positions, rotations, 2);
    ASSERT_EQ(contact_count(), 1u);
    EXPECT_EQ(contact(0).phase, ContactPhase::Exit);
    step(positions, rotations, 2);
    EXPECT_EQ(contact_count(), 0u);
}

TEST_F(PhysWorld, ATriggerReportsButNeverPushes)
{
    Collider trigger;
    trigger.kind = ColliderKind::Sphere;
    trigger.half_extents = v3(2.0f, 0, 0);
    trigger.is_trigger = true;
    add_collider(trigger);

    Collider mover;
    mover.kind = ColliderKind::Sphere;
    mover.half_extents = v3(0.5f, 0, 0);
    RigidBody b;
    b.use_gravity = false;
    mover.body = add_body(b);
    add_collider(mover);

    Vec3 positions[2] = {v3(0, 0, 0), v3(0.5f, 0, 0)};
    Quat rotations[2] = {quat_identity(), quat_identity()};
    step(positions, rotations, 2);

    ASSERT_EQ(contact_count(), 1u);
    EXPECT_TRUE(contact(0).is_trigger);
    EXPECT_FLOAT_EQ(positions[1].x, 0.5f)
        << "a trigger that moves things is not a trigger";
    EXPECT_FLOAT_EQ(body(0).velocity.x, 0.0f);
}

TEST_F(PhysWorld, TheLayerMatrixSuppressesAPairEntirely)
{
    Collider a;
    a.kind = ColliderKind::Sphere;
    a.half_extents = v3(1.0f, 0, 0);
    a.layer = 1;
    a.is_trigger = true;
    add_collider(a);

    Collider b = a;
    b.layer = 2;
    b.is_trigger = false;
    RigidBody rb;
    rb.use_gravity = false;
    b.body = add_body(rb);
    add_collider(b);

    Vec3 positions[2] = {v3(0, 0, 0), v3(1, 0, 0)};
    Quat rotations[2] = {quat_identity(), quat_identity()};

    step(positions, rotations, 2);
    EXPECT_EQ(contact_count(), 1u) << "layers 1 and 2 collide by default";

    set_layers_collide(1, 2, false);
    step(positions, rotations, 2);
    // The pair is gone, so the previous contact exits and then nothing.
    step(positions, rotations, 2);
    EXPECT_EQ(contact_count(), 0u);
}

TEST_F(PhysWorld, TwoDynamicBodiesPushApartAlongTheContactNormal)
{
    Collider a;
    a.kind = ColliderKind::Sphere;
    a.half_extents = v3(1.0f, 0, 0);
    RigidBody ra;
    ra.use_gravity = false;
    a.body = add_body(ra);
    add_collider(a);

    Collider b = a;
    RigidBody rb;
    rb.use_gravity = false;
    b.body = add_body(rb);
    add_collider(b);

    Vec3 positions[2] = {v3(-0.4f, 0, 0), v3(0.4f, 0, 0)}; // overlapping
    Quat rotations[2] = {quat_identity(), quat_identity()};
    const float before = positions[1].x - positions[0].x;
    step(positions, rotations, 2);
    const float after = positions[1].x - positions[0].x;

    EXPECT_GT(after, before) << "an overlapping pair must separate";
    // Equal masses, so each moves the same distance.
    EXPECT_NEAR(fabsf(positions[0].x), fabsf(positions[1].x), 1e-4f);
}

TEST_F(PhysWorld, AStaticColliderTakesNoneOfThePush)
{
    Collider wall; // no body: static to the solver
    wall.kind = ColliderKind::Box;
    wall.half_extents = v3(1, 1, 1);
    add_collider(wall);

    Collider ball;
    ball.kind = ColliderKind::Sphere;
    ball.half_extents = v3(0.5f, 0, 0);
    RigidBody rb;
    rb.use_gravity = false;
    ball.body = add_body(rb);
    add_collider(ball);

    Vec3 positions[2] = {v3(0, 0, 0), v3(1.2f, 0, 0)};
    Quat rotations[2] = {quat_identity(), quat_identity()};
    step(positions, rotations, 2);

    EXPECT_FLOAT_EQ(positions[0].x, 0.0f) << "the static collider moved";
    EXPECT_GT(positions[1].x, 1.2f) << "the dynamic one takes the whole push";
}

// ---- character controller --------------------------------------------------

TEST_F(PhysWorld, TheCharacterStandsOnTheFloorAndIsGrounded)
{
    Floor floor;
    floor.build(8, 4.0f);
    set_static_mesh(&floor.mesh);

    CharacterController cc;
    cc.radius = 0.4f;
    cc.height = 1.8f;
    const int32_t index = add_character(cc);
    ASSERT_GE(index, 0);

    Vec3 position = v3(0, 3, 0);
    // Fall until it lands.
    for (uint32_t i = 0; i < 60; ++i) {
        move_character(static_cast<uint32_t>(index), &position, v3(0, -0.2f, 0));
    }
    EXPECT_TRUE(character(0).grounded);
    // The capsule's lowest point is centre - height/2, which must rest at ~0.
    EXPECT_NEAR(position.y, 0.9f, 0.15f);
}

TEST_F(PhysWorld, TheCharacterIsStoppedByAWallAndSlidesAlongIt)
{
    Floor floor;
    floor.build(8, 4.0f);
    floor.add_wall(2.0f, 8.0f, 4.0f);
    floor.finish();
    set_static_mesh(&floor.mesh);

    CharacterController cc;
    cc.radius = 0.4f;
    cc.height = 1.8f;
    const uint32_t index = static_cast<uint32_t>(add_character(cc));

    Vec3 position = v3(0, 0.9f, 0);
    // Push diagonally into the wall: blocked on x, free on z.
    for (uint32_t i = 0; i < 30; ++i) {
        move_character(index, &position, v3(0.2f, 0, 0.2f));
    }
    EXPECT_LT(position.x, 2.0f) << "walked through the wall";
    EXPECT_GT(position.z, 2.0f) << "must still slide along the wall on z";
}

TEST_F(PhysWorld, TheCharacterClimbsAStepWithinItsStepOffset)
{
    Floor floor;
    floor.build(8, 4.0f);
    // A 0.25 m ledge: under the 0.3 m step offset, so it must be climbed.
    floor.add_wall(1.0f, 8.0f, 0.25f);
    floor.add_ledge(1.0f, 8.0f, 0.25f, 6.0f);
    floor.finish();
    set_static_mesh(&floor.mesh);

    CharacterController cc;
    cc.radius = 0.4f;
    cc.height = 1.8f;
    cc.step_offset = 0.3f;
    const uint32_t index = static_cast<uint32_t>(add_character(cc));

    Vec3 position = v3(0, 0.9f, 0);
    for (uint32_t i = 0; i < 40; ++i) {
        move_character(index, &position, v3(0.15f, -0.05f, 0));
    }
    EXPECT_GT(position.x, 1.5f) << "did not get onto the step";
    EXPECT_GT(position.y, 1.0f) << "did not rise onto the step surface";
}

TEST_F(PhysWorld, AStepTallerThanTheOffsetBlocksTheCharacter)
{
    Floor floor;
    floor.build(8, 4.0f);
    // 1.0 m: far above the 0.3 m offset, so it is a wall.
    floor.add_wall(1.0f, 8.0f, 1.0f);
    floor.add_ledge(1.0f, 8.0f, 1.0f, 6.0f);
    floor.finish();
    set_static_mesh(&floor.mesh);

    CharacterController cc;
    cc.radius = 0.4f;
    cc.height = 1.8f;
    cc.step_offset = 0.3f;
    const uint32_t index = static_cast<uint32_t>(add_character(cc));

    Vec3 position = v3(0, 0.9f, 0);
    for (uint32_t i = 0; i < 40; ++i) {
        move_character(index, &position, v3(0.15f, -0.05f, 0));
    }
    EXPECT_LT(position.x, 1.0f) << "climbed a step it should not have";
}

TEST_F(PhysWorld, ASlopeWithinTheLimitIsWalkableAndASteeperOneIsNot)
{
    {
        Floor gentle;
        gentle.add_ramp(30.0f, 8.0f, 10.0f); // under the 45 default
        gentle.finish();
        set_static_mesh(&gentle.mesh);

        CharacterController cc;
        cc.radius = 0.4f;
        cc.height = 1.8f;
        cc.slope_limit = 45.0f;
        const uint32_t index = static_cast<uint32_t>(add_character(cc));

        Vec3 position = v3(1.0f, 1.5f, 0);
        for (uint32_t i = 0; i < 40; ++i) {
            move_character(index, &position, v3(0.15f, -0.1f, 0));
        }
        EXPECT_TRUE(character(index).grounded)
            << "a 30 degree ramp is walkable at a 45 degree limit";
    }
    shutdown();
    init();
    {
        Floor steep;
        steep.add_ramp(70.0f, 8.0f, 10.0f); // well over the limit
        steep.finish();
        set_static_mesh(&steep.mesh);

        CharacterController cc;
        cc.radius = 0.4f;
        cc.height = 1.8f;
        cc.slope_limit = 45.0f;
        const uint32_t index = static_cast<uint32_t>(add_character(cc));

        Vec3 position = v3(1.0f, 4.0f, 0);
        for (uint32_t i = 0; i < 40; ++i) {
            move_character(index, &position, v3(0.15f, -0.1f, 0));
        }
        EXPECT_FALSE(character(index).grounded)
            << "a 70 degree face is a wall, not a floor";
    }
}

TEST_F(PhysWorld, AFastCharacterDoesNotTunnelThroughTheFloor)
{
    // The acceptance property, at the controller level: one enormous step.
    Floor floor;
    floor.build(8, 8.0f);
    set_static_mesh(&floor.mesh);

    CharacterController cc;
    cc.radius = 0.4f;
    cc.height = 1.8f;
    const uint32_t index = static_cast<uint32_t>(add_character(cc));

    Vec3 position = v3(0, 60.0f, 0);
    move_character(index, &position, v3(0, -120.0f, 0));
    EXPECT_GT(position.y, 0.0f)
        << "a 120 m step passed straight through the floor";
    EXPECT_TRUE(character(index).grounded);
}

TEST_F(PhysWorld, CollisionFlagsReportWhichSurfaceWasHit)
{
    Floor floor;
    floor.build(8, 4.0f);
    floor.add_wall(2.0f, 8.0f, 4.0f);
    floor.finish();
    set_static_mesh(&floor.mesh);

    CharacterController cc;
    cc.radius = 0.4f;
    cc.height = 1.8f;
    const uint32_t index = static_cast<uint32_t>(add_character(cc));

    Vec3 position = v3(0, 5, 0);
    uint32_t flags = 0;
    for (uint32_t i = 0; i < 60; ++i) {
        flags = move_character(index, &position, v3(0, -0.3f, 0));
    }
    EXPECT_TRUE((flags & kCollisionBelow) != 0u) << "landing must set Below";

    position = v3(1.0f, 0.9f, 0);
    for (uint32_t i = 0; i < 20; ++i) {
        flags = move_character(index, &position, v3(0.2f, 0, 0));
    }
    EXPECT_TRUE((flags & kCollisionSides) != 0u)
        << "walking into a wall must set Sides";
}

TEST_F(PhysWorld, TheCharacterVelocityIsTheAchievedOneNotTheRequestedOne)
{
    Floor floor;
    floor.build(8, 4.0f);
    floor.add_wall(1.0f, 8.0f, 4.0f);
    floor.finish();
    set_static_mesh(&floor.mesh);

    CharacterController cc;
    cc.radius = 0.4f;
    cc.height = 1.8f;
    const uint32_t index = static_cast<uint32_t>(add_character(cc));

    Vec3 position = v3(0, 0.9f, 0);
    for (uint32_t i = 0; i < 30; ++i) {
        move_character(index, &position, v3(0.3f, 0, 0));
    }
    // Hard against the wall: it asked for 0.3 and got nothing.
    EXPECT_NEAR(character(index).velocity.x, 0.0f, 1.0f)
        << "velocity must report what happened, as Unity's does";
}

// ---- determinism (M11 task 5) ----------------------------------------------

TEST_F(PhysWorld, TheSameInputsProduceBitIdenticalResultsAcrossRuns)
{
    // Determinism PER BUILD is the promise (not across builds, and not
    // against the Editor -- the EE's floats are not IEEE 754). The way to
    // test the promise that was actually made is to run the identical
    // simulation twice and compare exactly.
    auto run = []() {
        shutdown();
        init();
        static Floor floor;
        floor = Floor{};
        floor.build(8, 4.0f);
        set_static_mesh(&floor.mesh);

        for (uint32_t i = 0; i < 6; ++i) {
            Collider c;
            c.kind = ColliderKind::Sphere;
            c.half_extents = Vec3{0.5f, 0, 0};
            RigidBody b;
            b.restitution = 0.4f;
            b.velocity = Vec3{static_cast<float>(i) * 0.3f, 0.0f, -0.2f};
            c.body = add_body(b);
            add_collider(c);
        }
        Vec3 positions[6];
        Quat rotations[6];
        for (uint32_t i = 0; i < 6; ++i) {
            positions[i] = Vec3{static_cast<float>(i) * 0.9f, 4.0f, 0.0f};
            rotations[i] = quat_identity();
        }
        for (uint32_t s = 0; s < 120; ++s) {
            step(positions, rotations, 6);
        }
        std::vector<float> out;
        for (uint32_t i = 0; i < 6; ++i) {
            out.push_back(positions[i].x);
            out.push_back(positions[i].y);
            out.push_back(positions[i].z);
        }
        return out;
    };

    const std::vector<float> first = run();
    const std::vector<float> second = run();
    ASSERT_EQ(first.size(), second.size());
    for (size_t i = 0; i < first.size(); ++i) {
        // EXACT equality, not near: any difference at all means something
        // in the step depends on state that is not reset, and that is the
        // bug this test exists to catch.
        EXPECT_FLOAT_EQ(first[i], second[i]) << "component " << i << " diverged";
    }
}

TEST_F(PhysWorld, TheFixedTimestepIsClampedToSomethingSimulable)
{
    set_fixed_timestep(0.0f);
    EXPECT_GT(fixed_timestep(), 0.0f) << "a zero step would freeze time";
    set_fixed_timestep(-1.0f);
    EXPECT_GT(fixed_timestep(), 0.0f) << "a negative step would reverse it";
    set_fixed_timestep(100.0f);
    EXPECT_LE(fixed_timestep(), 0.2f);
    set_fixed_timestep(1.0f / 50.0f);
    EXPECT_FLOAT_EQ(fixed_timestep(), 1.0f / 50.0f);
}

TEST_F(PhysWorld, ContactOverflowIsCountedRatherThanSilentlyDropped)
{
    // A lost trigger enter is a bug the game will blame on itself, so the
    // runtime has to be able to say it happened.
    const uint32_t count = kMaxContacts + 8;
    std::vector<Vec3> positions;
    std::vector<Quat> rotations;
    for (uint32_t i = 0; i < count && i < kMaxColliders; ++i) {
        Collider c;
        c.kind = ColliderKind::Sphere;
        c.half_extents = v3(2.0f, 0, 0); // big enough that all overlap
        c.is_trigger = true;
        RigidBody b;
        b.use_gravity = false;
        b.is_kinematic = true;
        c.body = add_body(b);
        if (add_collider(c) < 0) {
            break;
        }
        positions.push_back(v3(0, 0, 0));
        rotations.push_back(quat_identity());
    }
    // Bodies run out before colliders do, so only the first kMaxBodies get
    // one; that is fine, the pairs among them still overflow the contact list.
    step(positions.data(), rotations.data(),
         static_cast<uint32_t>(positions.size()));
    EXPECT_EQ(contact_count(), kMaxContacts);
    EXPECT_GT(contacts_dropped(), 0u)
        << "the runtime must admit it dropped events";
}
