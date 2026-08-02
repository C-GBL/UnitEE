// .p2b reader + scene loader tests, including the M5 task 1 fuzz pass.
//
// The builder here constructs containers byte-for-byte per
// docs/formats/p2b-container.md -- independently of the C# writer, so a
// mismatch between either side and the spec shows up as a failing test on
// one of them rather than as garbage on the console.
#include "ps2ur/p2b.h"
#include "ps2ur/p2b_scene.h"

#include <gtest/gtest.h>

#include <cstring>
#include <random>
#include <vector>

using namespace ps2ur;
using namespace ps2ur::io;

namespace {

struct Builder {
    struct Section {
        uint32_t type;
        std::vector<uint8_t> payload;
        uint64_t name_hash = 0;
    };
    std::vector<Section> sections;

    static void put_u16(std::vector<uint8_t>& v, uint16_t x)
    {
        v.push_back(static_cast<uint8_t>(x));
        v.push_back(static_cast<uint8_t>(x >> 8));
    }
    static void put_u32(std::vector<uint8_t>& v, uint32_t x)
    {
        for (int i = 0; i < 4; ++i) {
            v.push_back(static_cast<uint8_t>(x >> (i * 8)));
        }
    }
    static void put_f32(std::vector<uint8_t>& v, float f)
    {
        union {
            float f;
            uint32_t u;
        } c{f};
        put_u32(v, c.u);
    }

    // Assembled per the spec: header, table, 2048-aligned payloads.
    std::vector<uint8_t> build()
    {
        const uint32_t count = static_cast<uint32_t>(sections.size());
        const uint32_t table_end = 32u + count * 32u;
        std::vector<uint32_t> offsets(count);
        uint32_t cursor = (table_end + 2047u) & ~2047u;
        for (uint32_t i = 0; i < count; ++i) {
            offsets[i] = cursor;
            cursor = (cursor + static_cast<uint32_t>(sections[i].payload.size()) +
                      2047u) &
                     ~2047u;
        }
        std::vector<uint8_t> out(cursor, 0);

        std::vector<uint8_t> h;
        put_u32(h, kP2bMagic);
        put_u16(h, 1);
        put_u16(h, 0);
        put_u32(h, cursor);
        put_u32(h, count);
        put_u32(h, 0);
        h.resize(32, 0);
        std::memcpy(out.data(), h.data(), 32);

        for (uint32_t i = 0; i < count; ++i) {
            std::vector<uint8_t> e;
            put_u32(e, sections[i].type);
            put_u32(e, offsets[i]);
            put_u32(e, static_cast<uint32_t>(sections[i].payload.size()));
            put_u32(e, static_cast<uint32_t>(sections[i].payload.size()));
            put_u32(e, crc32(sections[i].payload.data(),
                             static_cast<uint32_t>(sections[i].payload.size())));
            put_u32(e, 0);
            put_u32(e, static_cast<uint32_t>(sections[i].name_hash));
            put_u32(e, static_cast<uint32_t>(sections[i].name_hash >> 32));
            std::memcpy(out.data() + 32 + i * 32, e.data(), 32);
            std::memcpy(out.data() + offsets[i], sections[i].payload.data(),
                        sections[i].payload.size());
        }
        return out;
    }
};

// A minimal valid scene: one unlit mesh (one triangle), one material, two
// entities (parent + child with the mesh) and a camera on the parent.
Builder minimal_scene()
{
    Builder b;

    // MATL v2 (48-byte record, M8): kind, texture, colour, TEST u64,
    // ALPHA u64, flags (zwrite), pad.
    {
        std::vector<uint8_t> p;
        Builder::put_u32(p, scene::kMaterialUnlit);
        Builder::put_u32(p, 0xFFFFFFFFu);
        for (int i = 0; i < 4; ++i) {
            Builder::put_f32(p, 1.0f);
        }
        Builder::put_u32(p, 0); // TEST lo (0 = device default)
        Builder::put_u32(p, 0); // TEST hi
        Builder::put_u32(p, 0); // ALPHA lo
        Builder::put_u32(p, 0); // ALPHA hi
        Builder::put_u32(p, 1); // flags: zwrite
        Builder::put_u32(p, 0); // pad
        b.sections.push_back({kSectionMaterial, p});
    }

    // MESH: header + one batch desc + blob at qword offset 3.
    {
        std::vector<uint8_t> p;
        Builder::put_u32(p, 1);          // batch_count
        Builder::put_u32(p, 0);          // material_index
        for (int i = 0; i < 3; ++i) {
            Builder::put_f32(p, 0.0f);   // bounds centre
        }
        Builder::put_f32(p, 2.0f);       // bounds radius
        // desc: offset_qwords=3 (48 bytes: 24 header + 16 desc -> pad to 48)
        Builder::put_u32(p, 3);
        Builder::put_u32(p, 6);          // vert_qwords (3 verts * 2)
        Builder::put_u32(p, 3);          // vertex_count
        Builder::put_u32(p, 10);         // vert_dest
        p.resize(48, 0);
        // blob: tag qword (fields only loosely checked here), count, verts.
        std::vector<uint8_t> blob;
        Builder::put_u32(blob, 3u | (1u << 15)); // NLOOP=3 | EOP
        Builder::put_u32(blob, 0);
        Builder::put_u32(blob, 0x00008000u);     // FLG/NREG bits, unchecked
        Builder::put_u32(blob, 0x51u);
        Builder::put_u32(blob, 3);               // count qword
        blob.resize(32, 0);
        for (int vtx = 0; vtx < 3; ++vtx) {
            Builder::put_f32(blob, static_cast<float>(vtx));
            Builder::put_f32(blob, 0.0f);
            Builder::put_f32(blob, 0.0f);
            Builder::put_f32(blob, 1.0f);
            for (int c = 0; c < 4; ++c) {
                Builder::put_f32(blob, 128.0f);
            }
        }
        p.insert(p.end(), blob.begin(), blob.end());
        b.sections.push_back({kSectionMesh, p});
    }

    // SCEN: two entities + camera + mesh renderer.
    {
        std::vector<uint8_t> p;
        Builder::put_u32(p, 2); // entities
        Builder::put_u32(p, 2); // components
        Builder::put_u32(p, 0); // name table

        // entity 0: root with camera (component 0)
        Builder::put_u32(p, static_cast<uint32_t>(-1));
        Builder::put_f32(p, 1.0f);
        Builder::put_f32(p, 2.0f);
        Builder::put_f32(p, 3.0f);
        Builder::put_f32(p, 0);
        Builder::put_f32(p, 0);
        Builder::put_f32(p, 0);
        Builder::put_f32(p, 1.0f);
        Builder::put_f32(p, 1);
        Builder::put_f32(p, 1);
        Builder::put_f32(p, 1);
        Builder::put_u32(p, 0);
        Builder::put_u16(p, 0);
        Builder::put_u16(p, 0);
        Builder::put_u32(p, 0);
        Builder::put_u32(p, 0); // component_first
        Builder::put_u16(p, 1); // component_count
        Builder::put_u16(p, 0);

        // entity 1: child of 0, offset +2 in x, with the mesh (component 1)
        Builder::put_u32(p, 0);
        Builder::put_f32(p, 2.0f);
        Builder::put_f32(p, 0);
        Builder::put_f32(p, 0);
        Builder::put_f32(p, 0);
        Builder::put_f32(p, 0);
        Builder::put_f32(p, 0);
        Builder::put_f32(p, 1.0f);
        Builder::put_f32(p, 1);
        Builder::put_f32(p, 1);
        Builder::put_f32(p, 1);
        Builder::put_u32(p, 0);
        Builder::put_u16(p, 0);
        Builder::put_u16(p, 0);
        Builder::put_u32(p, 0);
        Builder::put_u32(p, 1);
        Builder::put_u16(p, 1);
        Builder::put_u16(p, 0);

        // component refs; payloads live after them.
        const uint32_t refs_at = static_cast<uint32_t>(p.size());
        const uint32_t payload0 = refs_at + 2u * 8u;      // camera: 12 bytes
        const uint32_t payload1 = payload0 + 12u;         // renderer: 8 bytes
        Builder::put_u16(p, scene::kComponentCamera);
        Builder::put_u16(p, 0);
        Builder::put_u32(p, payload0);
        Builder::put_u16(p, scene::kComponentMeshRenderer);
        Builder::put_u16(p, 0);
        Builder::put_u32(p, payload1);
        Builder::put_f32(p, 1.2f);
        Builder::put_f32(p, 0.5f);
        Builder::put_f32(p, 60.0f);
        Builder::put_u32(p, 0);           // mesh 0
        Builder::put_u32(p, 0xFFFFFFFFu); // no material override
        b.sections.push_back({kSectionScene, p});
    }
    return b;
}

std::vector<uint8_t> aligned_copy(const std::vector<uint8_t>& v,
                                  std::vector<uint8_t>& backing)
{
    backing.resize(v.size() + 16);
    (void)backing;
    return v; // std::vector data is sufficiently aligned on the host
}

} // namespace

TEST(P2bReader, ParsesAValidFile)
{
    auto bytes = minimal_scene().build();
    P2bFile f;
    ASSERT_TRUE(f.parse(bytes.data(), static_cast<uint32_t>(bytes.size())))
        << f.error();
    EXPECT_EQ(f.section_count(), 3u);
    EXPECT_EQ(f.count_of(kSectionMesh), 1u);
    ASSERT_NE(f.find(kSectionScene), nullptr);
    EXPECT_EQ(f.find(kSectionScene)->size % 4u, 0u);
}

TEST(P2bReader, RejectsBadMagicVersionSizeAndChecksum)
{
    auto bytes = minimal_scene().build();
    const uint32_t size = static_cast<uint32_t>(bytes.size());
    P2bFile f;

    auto corrupted = bytes;
    corrupted[0] ^= 0xFF;
    EXPECT_FALSE(f.parse(corrupted.data(), size));
    EXPECT_STREQ(f.error(), "bad magic");

    corrupted = bytes;
    corrupted[4] = 9;
    EXPECT_FALSE(f.parse(corrupted.data(), size));
    EXPECT_STREQ(f.error(), "unsupported version");

    EXPECT_FALSE(f.parse(bytes.data(), size - 1));
    EXPECT_STREQ(f.error(), "total_size does not match the buffer");

    corrupted = bytes;
    corrupted[2048 + 4] ^= 0x01; // inside the first section's payload -- the
                                 // file tail is alignment padding and flips
                                 // there are legitimately invisible
    EXPECT_FALSE(f.parse(corrupted.data(), size));
    EXPECT_STREQ(f.error(), "section checksum mismatch");

    // ...and the same byte flip passes with verification off, which is the
    // documented trade for trusted data.
    EXPECT_TRUE(f.parse(corrupted.data(), size, false));
}

TEST(P2bReader, FuzzedHeadersNeverCrash)
{
    // M5 task 1: "a fuzz test on the reader". 20k mutated files: parse() may
    // accept or reject, but must never read outside the buffer (ASAN-visible
    // on the host) and must never report success for a file whose payload
    // bounds are broken.
    auto pristine = minimal_scene().build();
    std::mt19937 rng(0xC0FFEE);
    std::uniform_int_distribution<uint32_t> pos_all(
        0, static_cast<uint32_t>(pristine.size()) - 1);
    // Bias half the mutations into the header+table where the parser logic is.
    std::uniform_int_distribution<uint32_t> pos_head(0, 127);
    std::uniform_int_distribution<uint32_t> byte(0, 255);

    uint32_t accepted = 0;
    for (int iter = 0; iter < 20000; ++iter) {
        auto fuzzed = pristine;
        const int flips = 1 + (iter % 4);
        for (int k = 0; k < flips; ++k) {
            const uint32_t at = (iter & 1) ? pos_head(rng) : pos_all(rng);
            fuzzed[at] = static_cast<uint8_t>(byte(rng));
        }
        P2bFile f;
        if (f.parse(fuzzed.data(), static_cast<uint32_t>(fuzzed.size()))) {
            accepted++;
            // Whatever was accepted must have in-bounds sections.
            for (uint32_t i = 0; i < f.section_count(); ++i) {
                const P2bSection& s = f.section(i);
                EXPECT_GE(s.data, fuzzed.data());
                EXPECT_LE(s.data + s.size, fuzzed.data() + fuzzed.size());
            }
        }
    }
    // Many flips legitimately survive: ~93% of a small container is
    // inter-section alignment padding, and name_hash/flags/reserved are not
    // semantically validated. The property under test is memory safety (the
    // in-bounds assertions above, ASAN-visible on the host), not an accept
    // rate. Still, SOMETHING must have been rejected, or validation is off.
    EXPECT_LT(accepted, 20000u);
    EXPECT_GT(accepted, 0u);

    // And the validated fields must always reject when actually changed.
    for (int k = 0; k < 4; ++k) {
        auto bad = pristine;
        bad[k] ^= 0xFF; // magic bytes
        P2bFile f2;
        EXPECT_FALSE(f2.parse(bad.data(), static_cast<uint32_t>(bad.size())));
    }
}

TEST(P2bScene, LoadsAndResolvesHierarchy)
{
    auto bytes = minimal_scene().build();
    P2bFile f;
    ASSERT_TRUE(f.parse(bytes.data(), static_cast<uint32_t>(bytes.size())))
        << f.error();

    scene::World world;
    ASSERT_TRUE(world.load(f)) << world.error();
    ASSERT_EQ(world.entity_count(), 2u);

    // Child world position = parent (1,2,3) + local (2,0,0).
    const Mat4& w1 = world.world_matrix(1);
    EXPECT_FLOAT_EQ(w1.m[12], 3.0f);
    EXPECT_FLOAT_EQ(w1.m[13], 2.0f);
    EXPECT_FLOAT_EQ(w1.m[14], 3.0f);

    EXPECT_EQ(world.entity(1).mesh, 0);
    ASSERT_TRUE(world.has_camera());
    EXPECT_EQ(world.camera().entity, 0);
    EXPECT_FLOAT_EQ(world.camera().fov, 1.2f);

    ASSERT_EQ(world.mesh_count(), 1u);
    const scene::LoadedMesh& mesh = world.mesh(0);
    ASSERT_EQ(mesh.batch_count, 1u);
    EXPECT_EQ(mesh.batches[0].vertex_count, 3u);
    EXPECT_EQ(mesh.batches[0].vert_dest, 10u);
    // Zero-copy: the batch points into the file buffer.
    EXPECT_GE(reinterpret_cast<const uint8_t*>(mesh.batches[0].header),
              bytes.data());
}

TEST(P2bScene, RejectsForwardParentReferences)
{
    // Build a scene whose entity 0 claims entity 1 as parent; the one-pass
    // world update would read a stale matrix, so load() must refuse.
    auto b = minimal_scene();
    for (auto& s : b.sections) {
        if (s.type == kSectionScene) {
            // entity 0's parent field is at offset 12 in the payload.
            s.payload[12] = 1;
            s.payload[13] = 0;
            s.payload[14] = 0;
            s.payload[15] = 0;
        }
    }
    auto bytes = b.build();
    P2bFile f;
    ASSERT_TRUE(f.parse(bytes.data(), static_cast<uint32_t>(bytes.size())));
    scene::World world;
    EXPECT_FALSE(world.load(f));
    EXPECT_STREQ(world.error(), "entity parent not before child");
}

TEST(P2bScene, FuzzedSectionsNeverCrashTheLoader)
{
    // Mutations that keep the container valid (checksums recomputed) but
    // corrupt SECTION CONTENT: the loader's bounds checks are the target.
    auto base = minimal_scene();
    std::mt19937 rng(0xBEEF);
    std::uniform_int_distribution<uint32_t> byte(0, 255);

    for (int iter = 0; iter < 4000; ++iter) {
        auto b = base;
        for (auto& s : b.sections) {
            if (s.payload.empty()) {
                continue;
            }
            std::uniform_int_distribution<uint32_t> pos(
                0, static_cast<uint32_t>(s.payload.size()) - 1);
            s.payload[pos(rng)] = static_cast<uint8_t>(byte(rng));
        }
        auto bytes = b.build();
        P2bFile f;
        ASSERT_TRUE(f.parse(bytes.data(), static_cast<uint32_t>(bytes.size())));
        scene::World world;
        (void)world.load(f); // accept or reject; must not fault
    }
    SUCCEED();
}

TEST(MathAdditions, TrsAndRigidInverse)
{
    const Quat r = quat_from_axis_angle(Vec3{0, 1, 0}, 1.57079632f);
    const Mat4 m = mat4_trs(Vec3{5, 0, 0}, r, Vec3{1, 1, 1});
    // A 90-degree yaw sends +x to -z (column-major, column vectors).
    const Vec4 p = mat4_mul_vec4(m, Vec4{1, 0, 0, 1});
    EXPECT_NEAR(p.x, 5.0f, 1e-4f);
    EXPECT_NEAR(p.z, -1.0f, 1e-4f);

    const Mat4 inv = mat4_rigid_inverse(m);
    const Vec4 back = mat4_mul_vec4(inv, p);
    EXPECT_NEAR(back.x, 1.0f, 1e-4f);
    EXPECT_NEAR(back.y, 0.0f, 1e-4f);
    EXPECT_NEAR(back.z, 0.0f, 1e-4f);
}
