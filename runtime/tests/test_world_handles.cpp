// M7 object model: generation-checked handles, runtime create/destroy/
// reparent, active chains, and the order-tolerant world-matrix pass
// (plan section 9 M7 task 3). These invariants back Unity's destroyed-
// compares-null semantics, so they get their own suite.
#include "ps2ur/p2b_scene.h"

#include <gtest/gtest.h>

using namespace ps2ur;
using ps2ur::scene::World;

TEST(WorldHandles, CreateResolveRoundtrip)
{
    static World world;
    const int32_t index = world.create_entity(-1);
    ASSERT_GE(index, 0);
    const int32_t handle = world.handle_of(index);
    ASSERT_NE(handle, 0);
    EXPECT_EQ(world.resolve(handle), index);
}

TEST(WorldHandles, ZeroAndGarbageHandlesResolveDead)
{
    static World world;
    EXPECT_EQ(world.resolve(0), -1);
    EXPECT_EQ(world.resolve(-1), -1);
    EXPECT_EQ(world.resolve(0x7FFFFFFF), -1);
    EXPECT_EQ(world.handle_of(-1), 0);
    EXPECT_EQ(world.handle_of(9999), 0);
}

TEST(WorldHandles, DestroyRetiresHandleEvenAfterSlotReuse)
{
    static World world;
    const int32_t first = world.create_entity(-1);
    const int32_t stale = world.handle_of(first);
    world.destroy_entity(first);
    EXPECT_EQ(world.resolve(stale), -1);

    // The slot comes back for a NEW entity; the old handle must stay dead.
    const int32_t second = world.create_entity(-1);
    EXPECT_EQ(second, first);
    EXPECT_EQ(world.resolve(stale), -1);
    EXPECT_EQ(world.resolve(world.handle_of(second)), second);
}

TEST(WorldHandles, DestroyCascadesToDescendants)
{
    static World world;
    const int32_t root = world.create_entity(-1);
    const int32_t child = world.create_entity(root);
    const int32_t grandchild = world.create_entity(child);
    const int32_t childHandle = world.handle_of(child);
    const int32_t grandHandle = world.handle_of(grandchild);

    world.destroy_entity(root);
    EXPECT_EQ(world.resolve(childHandle), -1);
    EXPECT_EQ(world.resolve(grandHandle), -1);
}

TEST(WorldHandles, VisibilityWalksTheActiveChain)
{
    static World world;
    const int32_t root = world.create_entity(-1);
    const int32_t child = world.create_entity(root);
    EXPECT_TRUE(world.entity_visible(child));

    world.entity_mut(static_cast<uint32_t>(root)).active = false;
    EXPECT_FALSE(world.entity_visible(child));   // inherited
    EXPECT_TRUE(world.entity(static_cast<uint32_t>(child)).active); // own flag intact

    world.entity_mut(static_cast<uint32_t>(root)).active = true;
    EXPECT_TRUE(world.entity_visible(child));
}

TEST(WorldHandles, WorldMatricesResolveOutOfOrderParents)
{
    static World world;
    // Create the CHILD first so its index precedes its future parent's:
    // exactly the forward reference the p2b loader forbids but runtime
    // reparenting can produce.
    const int32_t child = world.create_entity(-1);
    const int32_t parent = world.create_entity(-1);
    ASSERT_LT(child, parent);

    world.entity_mut(static_cast<uint32_t>(parent)).pos = Vec3{5.0f, 0.0f, 0.0f};
    world.entity_mut(static_cast<uint32_t>(child)).parent = parent;
    world.entity_mut(static_cast<uint32_t>(child)).pos = Vec3{0.0f, 3.0f, 0.0f};

    world.update_world_matrices();

    // Column-major Mat4: translation in m[12..14].
    const Mat4& m = world.world_matrix(static_cast<uint32_t>(child));
    EXPECT_FLOAT_EQ(m.m[12], 5.0f);
    EXPECT_FLOAT_EQ(m.m[13], 3.0f);
    EXPECT_FLOAT_EQ(m.m[14], 0.0f);
}

TEST(WorldHandles, DeadEntitiesAreSkippedByTheMatrixPass)
{
    static World world;
    const int32_t a = world.create_entity(-1);
    const int32_t b = world.create_entity(-1);
    world.entity_mut(static_cast<uint32_t>(b)).pos = Vec3{1.0f, 2.0f, 3.0f};
    world.destroy_entity(a);
    world.update_world_matrices(); // must not touch/require the dead slot
    const Mat4& m = world.world_matrix(static_cast<uint32_t>(b));
    EXPECT_FLOAT_EQ(m.m[12], 1.0f);
    EXPECT_FLOAT_EQ(m.m[13], 2.0f);
    EXPECT_FLOAT_EQ(m.m[14], 3.0f);
}
