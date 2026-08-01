// BatchBuilder tests (plan section 9, M4 task 6; block format per 10.3).
//
// The blocks are consumed by the VIF, which reports nothing on malformed
// input -- it just hangs or draws garbage. So every field the VIF parses is
// pinned here on the host, where a wrong NUM is a test failure instead of a
// wedged console.
#include "ps2ur/gs_batch.h"

#include <gtest/gtest.h>

#include <vector>

using namespace ps2ur;
using namespace ps2ur::gfx;

namespace {

struct Fx {
    std::vector<uint8_t> memory;
    Arena arena;
    Fx()
    {
        memory.resize(256 * 1024);
        // Arena base must be qword aligned for the block qwords.
        void* base = memory.data();
        uintptr_t p = reinterpret_cast<uintptr_t>(base);
        const uintptr_t aligned = (p + 15u) & ~uintptr_t(15);
        arena.init(reinterpret_cast<void*>(aligned), memory.size() - (aligned - p));
    }

    std::vector<UnlitVertex> tri_list(uint32_t verts)
    {
        std::vector<UnlitVertex> v(verts);
        for (uint32_t i = 0; i < verts; ++i) {
            v[i] = UnlitVertex{static_cast<float>(i), 1.0f, 2.0f, 10, 20, 30, 128};
        }
        return v;
    }
};

uint32_t word(const Qword& q, int i)
{
    switch (i) {
    case 0: return static_cast<uint32_t>(q.lo);
    case 1: return static_cast<uint32_t>(q.lo >> 32);
    case 2: return static_cast<uint32_t>(q.hi);
    default: return static_cast<uint32_t>(q.hi >> 32);
    }
}

} // namespace

TEST(VifCodes, EncodeExactly)
{
    EXPECT_EQ(vif_nop(), 0u);
    EXPECT_EQ(vif_stcycl(1, 1), 0x01000101u);
    // UNPACK V4-32: cmd 0x6C, num in [23:16], addr in [9:0].
    EXPECT_EQ(vif_unpack_v4_32(10, 186), 0x6CBA000Au);
    EXPECT_EQ(vif_mscal(0), 0x14000000u);
}

TEST(BatchBuilder, SingleBlockLayout)
{
    Fx fx;
    auto verts = fx.tri_list(6);
    BatchBlock blocks[4];
    const uint32_t n = BatchBuilder::build_unlit(verts.data(), 6, fx.arena, blocks, 4);
    ASSERT_EQ(n, 1u);
    ASSERT_EQ(blocks[0].vertex_count, 6u);
    ASSERT_EQ(blocks[0].vert_qwords, 12u);
    ASSERT_EQ(blocks[0].vert_dest, 10u);
    // Header and payload are contiguous: verts = header + 2.
    ASSERT_EQ(blocks[0].verts, blocks[0].header + 2);

    // Header qword 0 is the GIF tag: NLOOP=6, EOP, PACKED, NREG=2, RGBAQ+XYZ2.
    const Qword* h = blocks[0].header;
    EXPECT_EQ(h[0].lo & 0x7FFFu, 6u);
    EXPECT_EQ((h[0].lo >> 15) & 1u, 1u);
    EXPECT_EQ((h[0].lo >> 60) & 0xFu, 2u);
    EXPECT_EQ(h[0].hi & 0xFFu,
              static_cast<uint64_t>(GsReg::RGBAQ) |
                  (static_cast<uint64_t>(GsReg::XYZ2) << 4));

    // Header qword 1: the loop count the microprogram reads with ilw.x.
    EXPECT_EQ(h[1].lo & 0xFFFFFFFFu, 6u);
}

TEST(BatchBuilder, VertexPayloadRoundTrips)
{
    Fx fx;
    auto verts = fx.tri_list(3);
    verts[1] = UnlitVertex{-1.5f, 2.5f, -3.5f, 200, 100, 50, 64};
    BatchBlock blocks[2];
    ASSERT_EQ(BatchBuilder::build_unlit(verts.data(), 3, fx.arena, blocks, 2), 1u);

    const Qword* b = blocks[0].header;
    // Vertex 1 position at qword 2 + 1*2.
    union {
        float f;
        uint32_t u;
    } c;
    c.u = word(b[4], 0);
    EXPECT_FLOAT_EQ(c.f, -1.5f);
    c.u = word(b[4], 3);
    EXPECT_FLOAT_EQ(c.f, 1.0f) << "position w must be 1";
    // Vertex 1 colour: floats of the byte values.
    c.u = word(b[5], 0);
    EXPECT_FLOAT_EQ(c.f, 200.0f);
    c.u = word(b[5], 3);
    EXPECT_FLOAT_EQ(c.f, 64.0f);
}

TEST(BatchBuilder, SplitsAtCapacityOnTriangleBoundaries)
{
    Fx fx;
    const uint32_t total = 300; // 100 triangles > one batch
    auto verts = fx.tri_list(total);
    BatchBlock blocks[8];
    const uint32_t n = BatchBuilder::build_unlit(verts.data(), total, fx.arena, blocks, 8);
    ASSERT_GE(n, 2u);
    uint32_t sum = 0;
    for (uint32_t i = 0; i < n; ++i) {
        EXPECT_LE(blocks[i].vertex_count, kMaxUnlitVertsPerBatch);
        EXPECT_EQ(blocks[i].vertex_count % 3u, 0u) << "no split mid-triangle";
        // The VIF NUM field is 8 bits; >255 silently wraps, so pin it here.
        EXPECT_LE(blocks[i].vertex_count * 2u, 255u);
        sum += blocks[i].vertex_count;
    }
    EXPECT_EQ(sum, total) << "every vertex lands in exactly one block";
}

TEST(BatchBuilder, LitBlocksStayUnderVifNumLimit)
{
    Fx fx;
    std::vector<LitVertex> verts(240);
    for (auto& v : verts) {
        v = LitVertex{0, 0, 0, 0, 0, 1, 255, 255, 255, 128};
    }
    BatchBlock blocks[8];
    const uint32_t n = BatchBuilder::build_lit(verts.data(), 240, fx.arena, blocks, 8);
    ASSERT_GE(n, 2u);
    for (uint32_t i = 0; i < n; ++i) {
        EXPECT_LE(blocks[i].vertex_count, kMaxLitVertsPerBatch);
        EXPECT_LE(blocks[i].vertex_count * 3u, 255u) << "NUM is 8 bits";
        // Lit vertices unpack to VU address 18.
        EXPECT_EQ(blocks[i].vert_dest, 18u);
        EXPECT_EQ(blocks[i].vert_qwords, blocks[i].vertex_count * 3u);
    }
}

TEST(BatchBuilder, RejectsNonTriangleCounts)
{
    Fx fx;
    auto verts = fx.tri_list(6);
    BatchBlock blocks[2];
    EXPECT_EQ(BatchBuilder::build_unlit(verts.data(), 5, fx.arena, blocks, 2), 0u);
    EXPECT_EQ(BatchBuilder::build_unlit(verts.data(), 0, fx.arena, blocks, 2), 0u);
}

TEST(BatchBuilder, ConstantsMatchTheMicroprogramLayout)
{
    float mvp[16];
    for (int i = 0; i < 16; ++i) {
        mvp[i] = static_cast<float>(i);
    }
    float scale[3] = {256.0f, -224.0f, 1000.0f};
    float offset[3] = {2304.0f, 2272.0f, 1000.0f};
    Qword out[7];
    BatchBuilder::build_unlit_constants(mvp, scale, offset, 4095.0f, 0.0625f, out);

    union {
        float f;
        uint32_t u;
    } c;
    // Column 2, row 1 lives in qword 2 lane y.
    c.u = static_cast<uint32_t>(out[2].lo >> 32);
    EXPECT_FLOAT_EQ(c.f, 9.0f);
    // Clip qword: (4095, 4095, 0, near).
    c.u = static_cast<uint32_t>(out[6].lo);
    EXPECT_FLOAT_EQ(c.f, 4095.0f);
    c.u = static_cast<uint32_t>(out[6].hi >> 32);
    EXPECT_FLOAT_EQ(c.f, 0.0625f);
}
