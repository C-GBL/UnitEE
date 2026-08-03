// Rigidbody as a SCEN component (M11 / M12 gap fix).
//
// The collider half of a physics object travels in PHYS and is covered by
// test_phys_bake.cpp. This is the other half: the BODY, which rides in the
// scene section so the managed Rigidbody can be constructed from it at load
// and GetComponent<Rigidbody>() finds a real object.
//
// The SCEN bytes here are written from docs/formats/p2b-container.md rather
// than from the C# exporter, the same discipline the rest of the container
// tests use: a drift on either side fails a test instead of producing a
// scene where a body silently never simulates.
#include "ps2ur/p2b_scene.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using namespace ps2ur;
using namespace ps2ur::scene;

namespace {

void put_u16(std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}

void put_u32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 24));
}

void put_i32(std::vector<uint8_t>& v, int32_t x)
{
    put_u32(v, static_cast<uint32_t>(x));
}

void put_f32(std::vector<uint8_t>& v, float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    put_u32(v, bits);
}

// One component attached to one entity, for the tests below.
struct BodySpec {
    float mass = 1.0f;
    float linear_damping = 0.0f;
    float angular_damping = 0.05f;
    uint32_t flags = 1u;
};

// A SCEN payload holding 'entity_count' root entities at the origin, with a
// Rigidbody component on each entity named in 'bodies'.
std::vector<uint8_t> build_scene(uint32_t entity_count,
                                 const std::vector<uint32_t>& body_entities,
                                 const BodySpec& spec,
                                 uint32_t payload_bytes = 16u)
{
    std::vector<uint8_t> b;
    put_u32(b, entity_count);
    put_u32(b, static_cast<uint32_t>(body_entities.size()));
    put_u32(b, 0); // name table

    // Entity records are 64 bytes each; component_first indexes the ref
    // table, which follows them.
    uint32_t next_component = 0;
    for (uint32_t i = 0; i < entity_count; ++i) {
        bool has_body = false;
        for (uint32_t e : body_entities) {
            if (e == i) {
                has_body = true;
            }
        }
        put_i32(b, -1);       // parent: all roots
        put_f32(b, 0.0f);     // pos
        put_f32(b, 0.0f);
        put_f32(b, 0.0f);
        put_f32(b, 0.0f);     // rot
        put_f32(b, 0.0f);
        put_f32(b, 0.0f);
        put_f32(b, 1.0f);
        put_f32(b, 1.0f);     // scale
        put_f32(b, 1.0f);
        put_f32(b, 1.0f);
        put_u32(b, 0);        // name hash
        put_u16(b, 0);        // layer
        put_u16(b, 0);        // tag
        put_u32(b, 0);        // flags
        put_u32(b, next_component);
        put_u16(b, has_body ? 1 : 0);
        put_u16(b, 0);        // pad
        if (has_body) {
            ++next_component;
        }
    }

    const uint32_t refs_at = static_cast<uint32_t>(b.size());
    const uint32_t count = static_cast<uint32_t>(body_entities.size());
    uint32_t payload_at = refs_at + count * 8u;
    for (uint32_t i = 0; i < count; ++i) {
        put_u16(b, kComponentRigidbody);
        put_u16(b, 0);
        put_u32(b, payload_at);
        payload_at += payload_bytes;
    }
    for (uint32_t i = 0; i < count; ++i) {
        // A short payload is how a truncation is provoked; the loader must
        // refuse rather than read past the section.
        std::vector<uint8_t> one;
        put_f32(one, spec.mass);
        put_f32(one, spec.linear_damping);
        put_f32(one, spec.angular_damping);
        put_u32(one, spec.flags);
        one.resize(payload_bytes, 0);
        b.insert(b.end(), one.begin(), one.end());
    }
    return b;
}

// A single-section container around a SCEN payload, 2048-aligned like the
// real writer so the payload keeps its alignment guarantee.
std::vector<uint8_t> wrap_scene(const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> file(2048 + payload.size(), 0);
    const char magic[4] = {'P', '2', 'B', 'C'};
    std::memcpy(file.data(), magic, 4);
    file[4] = 1; // version_major
    const uint32_t total = static_cast<uint32_t>(file.size());
    std::memcpy(file.data() + 8, &total, 4);
    const uint32_t sections = 1;
    std::memcpy(file.data() + 12, &sections, 4);

    uint8_t* entry = file.data() + 32;
    const uint32_t type = io::kSectionScene;
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

} // namespace

TEST(SceneRigidbody, RoundTripsEveryFieldAndFlag)
{
    BodySpec spec;
    spec.mass = 42.5f;
    spec.linear_damping = 0.25f;
    spec.angular_damping = 0.75f;
    spec.flags = 1u | 4u; // gravity on, not kinematic, rotation frozen

    const std::vector<uint8_t> bytes = wrap_scene(build_scene(3, {1}, spec));
    io::P2bFile file;
    ASSERT_TRUE(file.parse(bytes.data(), static_cast<uint32_t>(bytes.size())));
    static World world;
    ASSERT_TRUE(world.load(file)) << world.error();

    ASSERT_EQ(world.rigidbody_count(), 1u);
    const RigidbodyRef& rb = world.rigidbody(0);
    EXPECT_EQ(rb.entity, 1);
    EXPECT_FLOAT_EQ(rb.mass, 42.5f);
    EXPECT_FLOAT_EQ(rb.linear_damping, 0.25f);
    EXPECT_FLOAT_EQ(rb.angular_damping, 0.75f);
    EXPECT_EQ(rb.flags & 1u, 1u);
    EXPECT_EQ(rb.flags & 2u, 0u);
    EXPECT_EQ(rb.flags & 4u, 4u);
}

TEST(SceneRigidbody, AnEntityWithoutOneContributesNothing)
{
    const std::vector<uint8_t> bytes = wrap_scene(build_scene(4, {}, BodySpec{}));
    io::P2bFile file;
    ASSERT_TRUE(file.parse(bytes.data(), static_cast<uint32_t>(bytes.size())));
    static World world;
    ASSERT_TRUE(world.load(file)) << world.error();
    EXPECT_EQ(world.entity_count(), 4u);
    EXPECT_EQ(world.rigidbody_count(), 0u);
}

TEST(SceneRigidbody, ATruncatedPayloadIsRefusedRatherThanRead)
{
    // 12 bytes where the format says 16: the flags word is missing. Reading
    // it would pull whatever followed the section into the simulation.
    const std::vector<uint8_t> bytes =
        wrap_scene(build_scene(2, {0}, BodySpec{}, /*payload_bytes=*/12u));
    io::P2bFile file;
    ASSERT_TRUE(file.parse(bytes.data(), static_cast<uint32_t>(bytes.size())));
    static World world;
    EXPECT_FALSE(world.load(file));
    EXPECT_STREQ(world.error(), "rigidbody payload truncated");
}

TEST(SceneRigidbody, ASecondLoadDoesNotInheritTheFirstScenesBodies)
{
    // The M10 lesson, applied here before it can bite: World::load reuses a
    // running world, so a counter that is not reset accumulates across
    // loads and eventually overflows its table (verify-log M10).
    const std::vector<uint8_t> bytes = wrap_scene(build_scene(2, {0, 1}, BodySpec{}));
    io::P2bFile file;
    ASSERT_TRUE(file.parse(bytes.data(), static_cast<uint32_t>(bytes.size())));

    static World world;
    ASSERT_TRUE(world.load(file)) << world.error();
    ASSERT_EQ(world.rigidbody_count(), 2u);
    ASSERT_TRUE(world.load(file)) << world.error();
    EXPECT_EQ(world.rigidbody_count(), 2u) << "load must reset, not accumulate";
}

TEST(SceneRigidbody, AdditiveLoadRebasesTheEntityIndex)
{
    const std::vector<uint8_t> bytes = wrap_scene(build_scene(3, {2}, BodySpec{}));
    io::P2bFile file;
    ASSERT_TRUE(file.parse(bytes.data(), static_cast<uint32_t>(bytes.size())));

    static World world;
    ASSERT_TRUE(world.load(file)) << world.error();
    ASSERT_EQ(world.rigidbody_count(), 1u);
    EXPECT_EQ(world.rigidbody(0).entity, 2);

    // Merging the same scene again puts its entity 2 at index 5, and its
    // body must follow it. A body left pointing at entity 2 would drive
    // the FIRST copy's transform -- two objects, one of them possessed.
    ASSERT_TRUE(world.append(file)) << world.error();
    ASSERT_EQ(world.rigidbody_count(), 2u);
    EXPECT_EQ(world.rigidbody(0).entity, 2);
    EXPECT_EQ(world.rigidbody(1).entity, 5);
}

TEST(SceneRigidbody, MoreBodiesThanTheTableHoldsIsALoadFailure)
{
    const uint32_t n = kMaxRigidbodies + 1u;
    std::vector<uint32_t> all;
    for (uint32_t i = 0; i < n; ++i) {
        all.push_back(i);
    }
    const std::vector<uint8_t> bytes = wrap_scene(build_scene(n, all, BodySpec{}));
    io::P2bFile file;
    ASSERT_TRUE(file.parse(bytes.data(), static_cast<uint32_t>(bytes.size())));
    static World world;
    EXPECT_FALSE(world.load(file));
    EXPECT_STREQ(world.error(), "too many rigidbodies");
}
