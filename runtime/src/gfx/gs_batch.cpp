#include "ps2ur/gs_batch.h"

#include "ps2ur/assert.h"
#include "ps2ur/log.h"

#include <cstring>

namespace ps2ur {
namespace gfx {

namespace {

void set_float4(Qword& q, float x, float y, float z, float w)
{
    union {
        float f;
        uint32_t u;
    } cx{x}, cy{y}, cz{z}, cw{w};
    q.lo = static_cast<uint64_t>(cx.u) | (static_cast<uint64_t>(cy.u) << 32);
    q.hi = static_cast<uint64_t>(cz.u) | (static_cast<uint64_t>(cw.u) << 32);
}

// Allocates a block: 2 header qwords (tag, count) + verts*qpv payload.
Qword* begin_block(Arena& arena, uint32_t verts, uint32_t qpv, uint32_t nreg,
                   uint64_t regs, uint64_t prim)
{
    Qword* block =
        static_cast<Qword*>(arena.alloc((2u + verts * qpv) * sizeof(Qword), 16));
    if (block == nullptr) {
        return nullptr;
    }
    GsPacket tag;
    tag.init(&block[0], 1);
    tag.begin_packed(verts, nreg, regs, /*eop=*/true, /*set_prim=*/true, prim);
    block[1].lo = verts;
    block[1].hi = 0;
    return block;
}

} // namespace

uint32_t BatchBuilder::build_unlit(const UnlitVertex* verts, uint32_t count,
                                   Arena& arena, BatchBlock* out_blocks,
                                   uint32_t max_blocks)
{
    if (verts == nullptr || count == 0 || count % 3u != 0 || out_blocks == nullptr) {
        return 0;
    }
    const uint64_t prim = gs_prim(GsPrim::Triangle, true, false, false, false,
                                  false, false, 0, false);
    const uint64_t regs = gs_reglist(GsReg::RGBAQ, GsReg::XYZ2);

    uint32_t block_count = 0;
    uint32_t done = 0;
    while (done < count) {
        uint32_t n = count - done;
        if (n > kMaxUnlitVertsPerBatch) {
            n = kMaxUnlitVertsPerBatch;
        }
        if (block_count >= max_blocks) {
            log(LogLevel::Error, "batch: more than %u blocks needed",
                static_cast<unsigned>(max_blocks));
            return 0;
        }
        Qword* block = begin_block(arena, n, 2, 2, regs, prim);
        if (block == nullptr) {
            log(LogLevel::Error, "batch: arena exhausted");
            return 0;
        }
        for (uint32_t i = 0; i < n; ++i) {
            const UnlitVertex& v = verts[done + i];
            set_float4(block[2 + i * 2], v.x, v.y, v.z, 1.0f);
            set_float4(block[3 + i * 2], static_cast<float>(v.r),
                       static_cast<float>(v.g), static_cast<float>(v.b),
                       static_cast<float>(v.a));
        }
        out_blocks[block_count] =
            BatchBlock{block, block + 2, n * 2u, n, /*vert_dest=*/10u};
        block_count++;
        done += n;
    }
    return block_count;
}

uint32_t BatchBuilder::build_lit(const LitVertex* verts, uint32_t count,
                                 Arena& arena, BatchBlock* out_blocks,
                                 uint32_t max_blocks)
{
    if (verts == nullptr || count == 0 || count % 3u != 0 || out_blocks == nullptr) {
        return 0;
    }
    const uint64_t prim = gs_prim(GsPrim::Triangle, true, false, false, false,
                                  false, false, 0, false);
    const uint64_t regs = gs_reglist(GsReg::RGBAQ, GsReg::XYZ2);

    uint32_t block_count = 0;
    uint32_t done = 0;
    while (done < count) {
        uint32_t n = count - done;
        if (n > kMaxLitVertsPerBatch) {
            n = kMaxLitVertsPerBatch;
        }
        if (block_count >= max_blocks) {
            return 0;
        }
        Qword* block = begin_block(arena, n, 3, 2, regs, prim);
        if (block == nullptr) {
            return 0;
        }
        for (uint32_t i = 0; i < n; ++i) {
            const LitVertex& v = verts[done + i];
            set_float4(block[2 + i * 3], v.x, v.y, v.z, 1.0f);
            set_float4(block[3 + i * 3], v.nx, v.ny, v.nz, 0.0f);
            set_float4(block[4 + i * 3], static_cast<float>(v.r),
                       static_cast<float>(v.g), static_cast<float>(v.b),
                       static_cast<float>(v.a));
        }
        out_blocks[block_count] =
            BatchBlock{block, block + 2, n * 3u, n, /*vert_dest=*/18u};
        block_count++;
        done += n;
    }
    return block_count;
}


uint32_t BatchBuilder::build_unlit_tex(const TexUnlitVertex* verts, uint32_t count,
                                       Arena& arena, BatchBlock* out_blocks,
                                       uint32_t max_blocks)
{
    if (verts == nullptr || count == 0 || count % 3u != 0 || out_blocks == nullptr) {
        return 0;
    }
    // Textured (TME) with STQ (FST=0): the tag order ST, RGBAQ, XYZ2 is what
    // makes each ST qword's Q pair with the following RGBAQ.
    const uint64_t prim = gs_prim(GsPrim::Triangle, true, /*textured=*/true,
                                  false, false, false, /*uv_is_st=*/false, 0,
                                  false);
    const uint64_t regs = gs_reglist(GsReg::ST, GsReg::RGBAQ, GsReg::XYZ2);

    uint32_t block_count = 0;
    uint32_t done = 0;
    while (done < count) {
        uint32_t n = count - done;
        if (n > kMaxTexVertsPerBatch) {
            n = kMaxTexVertsPerBatch;
        }
        if (block_count >= max_blocks) {
            return 0;
        }
        Qword* block = begin_block(arena, n, 3, 3, regs, prim);
        if (block == nullptr) {
            return 0;
        }
        for (uint32_t i = 0; i < n; ++i) {
            const TexUnlitVertex& v = verts[done + i];
            set_float4(block[2 + i * 3], v.x, v.y, v.z, 1.0f);
            // z = 1 becomes Q after the microprogram multiplies by 1/w.
            set_float4(block[3 + i * 3], v.u, v.v, 1.0f, 0.0f);
            set_float4(block[4 + i * 3], static_cast<float>(v.r),
                       static_cast<float>(v.g), static_cast<float>(v.b),
                       static_cast<float>(v.a));
        }
        out_blocks[block_count] =
            BatchBlock{block, block + 2, n * 3u, n, /*vert_dest=*/10u};
        block_count++;
        done += n;
    }
    return block_count;
}

void BatchBuilder::build_unlit_constants(const float mvp_columns[16],
                                         float viewport_scale[3],
                                         float viewport_offset[3], float guard_max,
                                         float w_near, Qword* out)
{
    PS2UR_ASSERT(mvp_columns != nullptr && out != nullptr);
    for (uint32_t c = 0; c < 4; ++c) {
        set_float4(out[c], mvp_columns[c * 4 + 0], mvp_columns[c * 4 + 1],
                   mvp_columns[c * 4 + 2], mvp_columns[c * 4 + 3]);
    }
    set_float4(out[4], viewport_scale[0], viewport_scale[1], viewport_scale[2], 0.0f);
    set_float4(out[5], viewport_offset[0], viewport_offset[1], viewport_offset[2],
               0.0f);
    set_float4(out[6], guard_max, guard_max, 0.0f, w_near);
}

} // namespace gfx
} // namespace ps2ur
