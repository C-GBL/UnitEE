// VU1 batch blocks and their builder (plan section 9, M4 task 6; data layout
// serving section 10.3).
//
// A batch block is PURE DATA in main RAM -- no VIF codes embedded:
//
//   qword 0 : the batch's GIF tag
//   qword 1 : the batch's vertex count (integer in x)
//   qword 2..: N vertices (2 qwords unlit, 3 qwords lit)
//
// The chain (dma_chain.h) references it with the SDK's proven pattern: one
// ref-tag unpack for the tag+count pair (VU address 7), one for the vertices
// (VU address 10 or 18), then FLUSH+MSCAL. Per-mesh constants (MVP, viewport,
// clip, lights: VU 0..6) are unpacked once, not per block, so blocks can be
// baked offline by the M5 exporter and referenced zero-copy from the loaded
// file. The VIF stalls MSCAL while the previous microprogram runs, so chained
// batches serialise with no EE involvement.
//
// Everything here is pure data construction and runs on the host, which is
// where the tests live.
#pragma once

#include "ps2ur/alloc.h"
#include "ps2ur/gs_packet.h"

#include <cstdint>

namespace ps2ur {
namespace gfx {

// VIF command words, kept for reference/tests and future custom streams
// (VIF code = [31:24 CMD][23:16 NUM][15:0 IMM]).
constexpr uint32_t vif_nop() { return 0; }
constexpr uint32_t vif_stcycl(uint32_t wl, uint32_t cl)
{
    return (0x01u << 24) | ((wl & 0xFFu) << 8) | (cl & 0xFFu);
}
constexpr uint32_t vif_unpack_v4_32(uint32_t dest, uint32_t num)
{
    return (0x6Cu << 24) | ((num & 0xFFu) << 16) | (dest & 0x3FFu);
}
constexpr uint32_t vif_mscal(uint32_t addr)
{
    return (0x14u << 24) | (addr & 0xFFFFu);
}

// Vertex layouts the builders consume. Colours are 0..255 (alpha 0..128,
// 0x80 = opaque); converted once here to the floats the VU expects.
struct UnlitVertex {
    float x, y, z;
    uint8_t r, g, b, a;
};
struct LitVertex {
    float x, y, z;
    float nx, ny, nz;
    uint8_t r, g, b, a;
};
struct TexUnlitVertex {
    float x, y, z;
    float u, v; // texture-normalised 0..1
    uint8_t r, g, b, a;
};

// One built block. header = tag+count (2 qwords), verts = payload.
struct BatchBlock {
    const Qword* header = nullptr;
    const Qword* verts = nullptr;
    uint32_t vert_qwords = 0;
    uint32_t vertex_count = 0;
    uint32_t vert_dest = 0; // VU data address the vertices unpack to
};

// Capacity limits from the microprogram data-memory layouts; both keep the
// 8-bit VIF NUM field under 256.
inline constexpr uint32_t kMaxUnlitVertsPerBatch = 93; // 2 qw/vert, dest 10
inline constexpr uint32_t kMaxLitVertsPerBatch = 78;   // 3 qw/vert, dest 18
inline constexpr uint32_t kMaxTexVertsPerBatch = 78;   // 3 qw/vert, dest 10
// Skinned vertices are 5 qwords (position, normal, colour, palette offsets,
// weights) and unpack to dest 114, above the 24-matrix bone palette. 48 x 5
// = 240 qwords keeps the 8-bit VIF NUM field valid -- the binding constraint
// here is NUM, not VU data memory (M9).
inline constexpr uint32_t kMaxSkinVertsPerBatch = 48;

class BatchBuilder {
public:
    // Splits a triangle list (count % 3 == 0) into blocks allocated from
    // 'arena'. Returns the number of blocks, or 0 on failure.
    static uint32_t build_unlit(const UnlitVertex* verts, uint32_t count,
                                Arena& arena, BatchBlock* out_blocks,
                                uint32_t max_blocks);
    static uint32_t build_lit(const LitVertex* verts, uint32_t count,
                              Arena& arena, BatchBlock* out_blocks,
                              uint32_t max_blocks);
    static uint32_t build_unlit_tex(const TexUnlitVertex* verts, uint32_t count,
                                    Arena& arena, BatchBlock* out_blocks,
                                    uint32_t max_blocks);

    // Per-mesh constant qwords for the unlit program (VU 0..6): MVP columns,
    // viewport scale/offset, clip constants. 'out' must hold 7 qwords.
    static void build_unlit_constants(const float mvp_columns[16],
                                      float viewport_scale[3], float viewport_offset[3],
                                      float guard_max, float w_near, Qword* out);
};

} // namespace gfx
} // namespace ps2ur
