// PHYS section round trip (M11 task 1).
//
// The writer here builds the section byte-for-byte from
// docs/formats/p2b-container.md, INDEPENDENTLY of the C# exporter -- the
// same discipline test_p2b.cpp uses. A disagreement between either side and
// the spec then shows up as a failing test on that side, rather than as
// collision that is quietly wrong in one corner of a level.
#include "ps2ur/phys.h"
#include "ps2ur/phys_bake.h"
#include "ps2ur/phys_bvh.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using namespace ps2ur;
using namespace ps2ur::phys;

namespace {

Vec3 v3(float x, float y, float z) { return Vec3{x, y, z}; }

void put_u32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 24));
}

void put_f32(std::vector<uint8_t>& v, float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    put_u32(v, bits);
}

void put_vec3(std::vector<uint8_t>& v, Vec3 p)
{
    put_f32(v, p.x);
    put_f32(v, p.y);
    put_f32(v, p.z);
}

// Builds a PHYS payload per the spec. Returned by value; the caller keeps it
// alive because the runtime reads the BVH in place.
struct PhysSection {
    std::vector<uint8_t> bytes;
    std::vector<Vec3> vertices;
    std::vector<BvhTriangle> triangles;
    std::vector<BvhNode> nodes;

    void add_quad(float y, float half)
    {
        const uint16_t base = static_cast<uint16_t>(vertices.size());
        vertices.push_back(v3(-half, y, -half));
        vertices.push_back(v3(half, y, -half));
        vertices.push_back(v3(-half, y, half));
        vertices.push_back(v3(half, y, half));
        triangles.push_back(BvhTriangle{base, static_cast<uint16_t>(base + 2),
                                        static_cast<uint16_t>(base + 1), 0, 0});
        triangles.push_back(BvhTriangle{static_cast<uint16_t>(base + 1),
                                        static_cast<uint16_t>(base + 2),
                                        static_cast<uint16_t>(base + 3), 0, 0});
    }

    void build(const std::vector<Collider>& colliders)
    {
        nodes.assign(bvh_max_nodes(static_cast<uint32_t>(triangles.size())),
                     BvhNode{});
        const uint32_t node_count = build_bvh(
            triangles.data(), static_cast<uint32_t>(triangles.size()),
            vertices.data(), static_cast<uint32_t>(vertices.size()),
            nodes.data(), static_cast<uint32_t>(nodes.size()));
        nodes.resize(node_count);

        const uint32_t header = 32;
        const uint32_t colliders_at = header;
        const uint32_t nodes_at =
            colliders_at + static_cast<uint32_t>(colliders.size()) * 48u;
        const uint32_t triangles_at = nodes_at + node_count * 32u;
        const uint32_t vertices_at =
            triangles_at + static_cast<uint32_t>(triangles.size()) * 8u;

        bytes.clear();
        put_u32(bytes, static_cast<uint32_t>(colliders.size()));
        put_u32(bytes, node_count);
        put_u32(bytes, static_cast<uint32_t>(triangles.size()));
        put_u32(bytes, static_cast<uint32_t>(vertices.size()));
        put_u32(bytes, colliders_at);
        put_u32(bytes, nodes_at);
        put_u32(bytes, triangles_at);
        put_u32(bytes, vertices_at);

        for (const Collider& c : colliders) {
            put_u32(bytes, static_cast<uint32_t>(c.kind));
            uint32_t flags = 0;
            if (c.is_trigger) {
                flags |= 1u;
            }
            if (c.enabled) {
                flags |= 2u;
            }
            flags |= (static_cast<uint32_t>(c.axis) & 3u) << 2;
            put_u32(bytes, flags);
            put_u32(bytes, c.layer);
            put_u32(bytes, static_cast<uint32_t>(c.entity));
            put_vec3(bytes, c.center);
            put_vec3(bytes, c.half_extents);
            put_f32(bytes, c.height);
            put_u32(bytes, 0); // reserved
        }
        for (const BvhNode& n : nodes) {
            put_vec3(bytes, n.bmin);
            put_u32(bytes, n.first);
            put_vec3(bytes, n.bmax);
            put_u32(bytes, n.count);
        }
        for (const BvhTriangle& t : triangles) {
            bytes.push_back(static_cast<uint8_t>(t.v0));
            bytes.push_back(static_cast<uint8_t>(t.v0 >> 8));
            bytes.push_back(static_cast<uint8_t>(t.v1));
            bytes.push_back(static_cast<uint8_t>(t.v1 >> 8));
            bytes.push_back(static_cast<uint8_t>(t.v2));
            bytes.push_back(static_cast<uint8_t>(t.v2 >> 8));
            bytes.push_back(t.layer);
            bytes.push_back(t.flags);
        }
        for (const Vec3& p : vertices) {
            put_vec3(bytes, p);
        }
    }
};

// A minimal single-section container, 2048-aligned like the real writer.
std::vector<uint8_t> wrap_container(const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> file(2048 + payload.size(), 0);
    const char magic[4] = {'P', '2', 'B', 'C'};
    std::memcpy(file.data(), magic, 4);
    file[4] = 1; // version_major
    file[5] = 0;
    file[6] = 0; // version_minor
    file[7] = 0;
    const uint32_t total = static_cast<uint32_t>(file.size());
    std::memcpy(file.data() + 8, &total, 4);
    const uint32_t count = 1;
    std::memcpy(file.data() + 12, &count, 4);

    uint8_t* entry = file.data() + 32;
    const uint32_t type = io::kSectionPhysics;
    const uint32_t offset = 2048;
    const uint32_t size = static_cast<uint32_t>(payload.size());
    const uint32_t checksum = io::crc32(payload.data(), size);
    std::memcpy(entry + 0, &type, 4);
    std::memcpy(entry + 4, &offset, 4);
    std::memcpy(entry + 8, &size, 4);
    std::memcpy(entry + 12, &size, 4);
    std::memcpy(entry + 16, &checksum, 4);
    std::memcpy(file.data() + 2048, payload.data(), payload.size());
    return file;
}

struct PhysBake : public ::testing::Test {
    void SetUp() override
    {
        shutdown();
        init();
    }
    void TearDown() override { shutdown(); }
};

} // namespace

TEST_F(PhysBake, LoadsCollidersAndAUsableBvhFromAContainer)
{
    PhysSection section;
    section.add_quad(0.0f, 10.0f);

    std::vector<Collider> colliders;
    Collider sphere;
    sphere.kind = ColliderKind::Sphere;
    sphere.half_extents = v3(1.5f, 0, 0);
    sphere.center = v3(0, 2, 0);
    sphere.layer = 4;
    sphere.entity = 7;
    colliders.push_back(sphere);

    Collider trigger;
    trigger.kind = ColliderKind::Capsule;
    trigger.half_extents = v3(0.5f, 0, 0);
    trigger.height = 3.0f;
    trigger.axis = CapsuleAxis::Z;
    trigger.is_trigger = true;
    trigger.entity = 9;
    // A layer of its own, so the masked query below isolates the static mesh
    // rather than finding this capsule sitting on top of it.
    trigger.layer = 5;
    colliders.push_back(trigger);

    section.build(colliders);
    const std::vector<uint8_t> file = wrap_container(section.bytes);

    io::P2bFile parsed;
    ASSERT_TRUE(parsed.parse(file.data(), static_cast<uint32_t>(file.size())))
        << parsed.error();

    StaticMesh mesh;
    BakeInfo info;
    const char* error = "";
    ASSERT_TRUE(load_physics(parsed, &mesh, &info, &error)) << error;

    EXPECT_EQ(info.collider_count, 2u);
    EXPECT_EQ(info.triangle_count, 2u);
    EXPECT_EQ(info.vertex_count, 4u);
    EXPECT_EQ(collider_count(), 2u);

    // Field-by-field, because a silently transposed field is exactly the bug
    // an independent writer is here to catch.
    EXPECT_EQ(collider_const(0).kind, ColliderKind::Sphere);
    EXPECT_FLOAT_EQ(collider_const(0).half_extents.x, 1.5f);
    EXPECT_FLOAT_EQ(collider_const(0).center.y, 2.0f);
    EXPECT_EQ(collider_const(0).layer, 4);
    EXPECT_EQ(collider_const(0).entity, 7);
    EXPECT_FALSE(collider_const(0).is_trigger);

    EXPECT_EQ(collider_const(1).kind, ColliderKind::Capsule);
    EXPECT_TRUE(collider_const(1).is_trigger);
    EXPECT_EQ(collider_const(1).axis, CapsuleAxis::Z);
    EXPECT_FLOAT_EQ(collider_const(1).height, 3.0f);
    EXPECT_EQ(collider_const(1).body, -1)
        << "the file must not claim a body index";

    // And the loaded data must actually answer queries -- both halves of it.
    set_static_mesh(&mesh);
    RaycastHit hit;

    // Masked to the triangles' layer (0), the floor at y=0 is what is found.
    ASSERT_TRUE(raycast(v3(0, 5, 0), v3(0, -1, 0), 100.0f, 1u << 0, &hit));
    EXPECT_NEAR(hit.distance, 5.0f, 1e-3f);
    EXPECT_EQ(hit.collider, -1) << "a static hit names no collider";

    // Unmasked, the loaded sphere is nearer: centred at y=2 with radius 1.5,
    // its top is at 3.5, so a ray from y=5 travels 1.5 to reach it. Getting
    // this back is what proves the collider's centre and radius survived the
    // round trip in the right fields.
    ASSERT_TRUE(raycast(v3(0, 5, 0), v3(0, -1, 0), 100.0f, kAllLayers, &hit));
    EXPECT_NEAR(hit.distance, 1.5f, 1e-3f);
    EXPECT_EQ(hit.collider, 0);
}

TEST_F(PhysBake, AContainerWithNoPhysicsSectionIsNotAnError)
{
    // Not every scene has collision, and refusing to load one would be wrong.
    std::vector<uint8_t> file(2048, 0);
    const char magic[4] = {'P', '2', 'B', 'C'};
    std::memcpy(file.data(), magic, 4);
    file[4] = 1;
    const uint32_t total = static_cast<uint32_t>(file.size());
    std::memcpy(file.data() + 8, &total, 4);
    const uint32_t count = 0;
    std::memcpy(file.data() + 12, &count, 4);

    io::P2bFile parsed;
    ASSERT_TRUE(parsed.parse(file.data(), static_cast<uint32_t>(file.size())));

    StaticMesh mesh;
    BakeInfo info;
    const char* error = "";
    EXPECT_TRUE(load_physics(parsed, &mesh, &info, &error)) << error;
    EXPECT_EQ(info.collider_count, 0u);
    EXPECT_EQ(mesh.node_count, 0u);
}

TEST_F(PhysBake, AnOffsetPointingOutsideTheSectionIsRefused)
{
    PhysSection section;
    section.add_quad(0.0f, 10.0f);
    section.build({});
    // Corrupt the vertices offset the way a broken exporter would.
    const uint32_t bad = 0xFFFF0000u;
    std::memcpy(section.bytes.data() + 28, &bad, 4);

    const std::vector<uint8_t> file = wrap_container(section.bytes);
    io::P2bFile parsed;
    ASSERT_TRUE(parsed.parse(file.data(), static_cast<uint32_t>(file.size())));

    StaticMesh mesh;
    BakeInfo info;
    const char* error = "";
    EXPECT_FALSE(load_physics(parsed, &mesh, &info, &error));
    EXPECT_STRNE(error, "");
    EXPECT_EQ(collider_count(), 0u)
        << "a rejected file must not have added anything";
}

TEST_F(PhysBake, ACorruptBvhIsRejectedRatherThanTraversed)
{
    PhysSection section;
    for (uint32_t i = 0; i < 8; ++i) {
        section.add_quad(static_cast<float>(i), 4.0f);
    }
    section.build({});

    // Point a leaf past the end of the triangle array. Traversing this would
    // read whatever the loader put next in the buffer.
    const uint32_t nodes_at = 32;
    for (uint32_t i = 0; i < section.nodes.size(); ++i) {
        uint32_t count = 0;
        std::memcpy(&count, section.bytes.data() + nodes_at + i * 32 + 28, 4);
        if (count > 0) {
            const uint32_t bad_first = 9999;
            std::memcpy(section.bytes.data() + nodes_at + i * 32 + 12,
                        &bad_first, 4);
            break;
        }
    }

    const std::vector<uint8_t> file = wrap_container(section.bytes);
    io::P2bFile parsed;
    ASSERT_TRUE(parsed.parse(file.data(), static_cast<uint32_t>(file.size())));

    StaticMesh mesh;
    BakeInfo info;
    const char* error = "";
    EXPECT_FALSE(load_physics(parsed, &mesh, &info, &error));
    EXPECT_STRNE(error, "");
}

TEST_F(PhysBake, TooManyCollidersIsRefusedBeforeAnythingIsAdded)
{
    PhysSection section;
    section.add_quad(0.0f, 1.0f);
    std::vector<Collider> colliders;
    for (uint32_t i = 0; i < kMaxColliders + 1; ++i) {
        Collider c;
        c.kind = ColliderKind::Sphere;
        c.half_extents = v3(1, 0, 0);
        colliders.push_back(c);
    }
    section.build(colliders);

    const std::vector<uint8_t> file = wrap_container(section.bytes);
    io::P2bFile parsed;
    ASSERT_TRUE(parsed.parse(file.data(), static_cast<uint32_t>(file.size())));

    StaticMesh mesh;
    BakeInfo info;
    const char* error = "";
    EXPECT_FALSE(load_physics(parsed, &mesh, &info, &error));
    EXPECT_EQ(collider_count(), 0u)
        << "the table must be untouched when the file does not fit";
}

TEST_F(PhysBake, PrimitiveOnlyCollisionCarriesNoBvhAtAll)
{
    // The shape a real scene takes: some box colliders, no mesh collider,
    // therefore no triangles.
    //
    // This pins the ONE way an empty tree may be encoded. count == 0 means
    // "internal node" in this format, so a single node with count 0 claims
    // two children that do not exist, and validate_bvh rejects the section.
    // There is no way to spell an empty leaf, so an empty tree is zero
    // nodes -- and the C# baker got this wrong in exactly that way, which
    // no test caught because nothing had ever called it (verify-log M12).
    PhysSection section;   // no add_quad: zero triangles, zero vertices
    std::vector<Collider> colliders;
    Collider box;
    box.kind = ColliderKind::Box;
    box.half_extents = v3(0.5f, 0.5f, 0.5f);
    box.entity = 1;
    colliders.push_back(box);
    section.build(colliders);

    const std::vector<uint8_t> file = wrap_container(section.bytes);
    io::P2bFile parsed;
    ASSERT_TRUE(parsed.parse(file.data(), static_cast<uint32_t>(file.size())));

    StaticMesh mesh;
    BakeInfo info;
    const char* error = "";
    ASSERT_TRUE(load_physics(parsed, &mesh, &info, &error)) << error;
    EXPECT_STREQ(error, "");
    EXPECT_EQ(info.collider_count, 1u);
    EXPECT_EQ(info.node_count, 0u);
    EXPECT_EQ(mesh.node_count, 0u);
    EXPECT_EQ(collider_count(), 1u);

    // And the world still answers queries with no static mesh to walk: the
    // box collider is still hit, and a ray pointed away from it misses
    // rather than dereferencing a null node array.
    set_static_mesh(&mesh);
    RaycastHit hit;
    EXPECT_TRUE(raycast(v3(0, 100, 0), v3(0, -1, 0), 1000.0f, 0xFFFFFFFFu, &hit))
        << "the primitive collider is still there";
    EXPECT_EQ(hit.collider, 0);
    EXPECT_FALSE(raycast(v3(50, 100, 50), v3(0, -1, 0), 1000.0f, 0xFFFFFFFFu, &hit))
        << "nothing to hit, and no BVH to walk";
}
