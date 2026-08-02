// GIF packet construction (plan section 9, M2 task 2).
//
// "This class is used everywhere afterwards; spend the time to get its
// ergonomics right." Everything the EE sends to the GS is a qword stream, so
// this is the type the whole renderer is written in terms of.
//
// The builder is deliberately non-owning: it writes into memory you give it,
// which at M4 will be a DMA chain buffer in scratchpad (plan section 3.4)
// rather than main RAM. It never allocates and never throws; overflow is a
// sticky flag you check once, not a per-write branch you have to handle.
#pragma once

#include "ps2ur/gs_format.h"

#include <cstdint>

namespace ps2ur {
namespace gfx {

// 128-bit quadword. Everything the GIF consumes is a stream of these, and all
// DMA on this machine works in qword units.
struct alignas(16) Qword {
    uint64_t lo;
    uint64_t hi;
};
static_assert(sizeof(Qword) == 16, "Qword must be exactly 128 bits");
static_assert(alignof(Qword) == 16, "Qword must be qword-aligned for DMA");

// GS register addresses, as written in the A+D field of a PACKED GIF packet.
// These are hardware encodings; do not renumber.
enum class GsReg : uint8_t {
    PRIM       = 0x00,
    RGBAQ      = 0x01,
    ST         = 0x02,
    UV         = 0x03,
    XYZF2      = 0x04,
    XYZ2       = 0x05,
    TEX0_1     = 0x06,
    TEX0_2     = 0x07,
    CLAMP_1    = 0x08,
    CLAMP_2    = 0x09,
    FOG        = 0x0A,
    TEX1_1     = 0x14,
    TEX1_2     = 0x15,
    XYOFFSET_1 = 0x18,
    XYOFFSET_2 = 0x19,
    PRMODECONT = 0x1A,
    PRMODE     = 0x1B,
    TEXCLUT    = 0x1C,
    TEXA       = 0x3B,
    FOGCOL     = 0x3D,
    TEXFLUSH   = 0x3F,
    SCISSOR_1  = 0x40,
    SCISSOR_2  = 0x41,
    ALPHA_1    = 0x42,
    ALPHA_2    = 0x43,
    DIMX       = 0x44,
    DTHE       = 0x45,
    COLCLAMP   = 0x46,
    TEST_1     = 0x47,
    TEST_2     = 0x48,
    PABE       = 0x49,
    FBA_1      = 0x4A,
    FRAME_1    = 0x4C,
    FRAME_2    = 0x4D,
    ZBUF_1     = 0x4E,
    ZBUF_2     = 0x4F,
    BITBLTBUF  = 0x50,
    TRXPOS     = 0x51,
    TRXREG     = 0x52,
    TRXDIR     = 0x53,
    HWREG      = 0x54,
    SIGNAL     = 0x60,
    FINISH     = 0x61,
    LABEL      = 0x62,
    NOP        = 0x7F,
};

// GIF primitive types (PRIM.PRIM / GIFtag PRIM field).
enum class GsPrim : uint8_t {
    Point         = 0,
    Line          = 1,
    LineStrip     = 2,
    Triangle      = 3,
    TriangleStrip = 4,
    TriangleFan   = 5,
    Sprite        = 6,
};

// The A+D descriptor for the GIFtag REGS field. This is NOT a GS register: it
// is the 4-bit code meaning "each following qword carries its own destination
// address in its high half". ps2sdk spells it GIF_REG_AD. Do not confuse it
// with GsReg::HWREG (0x54), which is the GS register used for image transfer;
// they live in different namespaces and swapping them produces garbage the GS
// will happily rasterise.
inline constexpr uint8_t kGifRegAD = 0x0E;

// GIFtag FLG field: how the data qwords after the tag are interpreted.
enum class GifMode : uint8_t {
    Packed  = 0, // register-per-field; the usual mode
    Reglist = 1,
    Image   = 2, // raw image data, used for texture upload
    Disable = 3,
};

// The GS screen origin. XYOFFSET is conventionally set so that screen (0,0)
// maps to 2048 in the GS's 12.4 fixed-point coordinate space, which keeps the
// usable area centred in the 16-bit coordinate range and leaves room for
// off-screen geometry to be clipped rather than wrapping.
inline constexpr uint32_t kGsOriginX = 2048u;
inline constexpr uint32_t kGsOriginY = 2048u;

// Screen pixels -> GS 12.4 fixed point, including the origin bias.
constexpr uint32_t gs_coord(int32_t pixels)
{
    return static_cast<uint32_t>((pixels << 4) + static_cast<int32_t>(kGsOriginX << 4));
}
// Sub-pixel variant: 'sixteenths' is already in 1/16th-pixel units.
constexpr uint32_t gs_coord_fixed(int32_t sixteenths)
{
    return static_cast<uint32_t>(sixteenths + static_cast<int32_t>(kGsOriginX << 4));
}

// ---- Register value builders ----------------------------------------------
// Free functions rather than methods so they can be used to precompute GS
// register blocks at export time (plan section 9, M8 task 5: "GS register
// blocks precomputed at export time, so switching material is a memcpy").

constexpr uint64_t gs_prim(GsPrim prim, bool gouraud, bool textured, bool fog,
                           bool alpha_blend, bool antialias, bool uv_is_st,
                           uint32_t context, bool fix_fragment)
{
    return (static_cast<uint64_t>(prim) << 0) | (static_cast<uint64_t>(gouraud) << 3) |
           (static_cast<uint64_t>(textured) << 4) | (static_cast<uint64_t>(fog) << 5) |
           (static_cast<uint64_t>(alpha_blend) << 6) | (static_cast<uint64_t>(antialias) << 7) |
           (static_cast<uint64_t>(uv_is_st) << 8) | (static_cast<uint64_t>(context) << 9) |
           (static_cast<uint64_t>(fix_fragment) << 10);
}

constexpr uint64_t gs_rgbaq(uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint32_t q_bits)
{
    return static_cast<uint64_t>(r) | (static_cast<uint64_t>(g) << 8) |
           (static_cast<uint64_t>(b) << 16) | (static_cast<uint64_t>(a) << 24) |
           (static_cast<uint64_t>(q_bits) << 32);
}

constexpr uint64_t gs_xyz(uint32_t x, uint32_t y, uint32_t z)
{
    return static_cast<uint64_t>(x) | (static_cast<uint64_t>(y) << 16) |
           (static_cast<uint64_t>(z) << 32);
}

constexpr uint64_t gs_frame(uint32_t base_page, uint32_t width_units, PixelFormat fmt,
                            uint32_t write_mask)
{
    return static_cast<uint64_t>(base_page) | (static_cast<uint64_t>(width_units) << 16) |
           (static_cast<uint64_t>(fmt) << 24) | (static_cast<uint64_t>(write_mask) << 32);
}

constexpr uint64_t gs_zbuf(uint32_t base_page, PixelFormat fmt, bool mask_write)
{
    // ZBUF.PSM stores only the low 4 bits of the Z format code.
    return static_cast<uint64_t>(base_page) |
           ((static_cast<uint64_t>(fmt) & 0x0Fu) << 24) |
           (static_cast<uint64_t>(mask_write) << 32);
}

constexpr uint64_t gs_xyoffset(uint32_t ox_fixed, uint32_t oy_fixed)
{
    return static_cast<uint64_t>(ox_fixed) | (static_cast<uint64_t>(oy_fixed) << 32);
}

constexpr uint64_t gs_scissor(uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1)
{
    return static_cast<uint64_t>(x0) | (static_cast<uint64_t>(x1) << 16) |
           (static_cast<uint64_t>(y0) << 32) | (static_cast<uint64_t>(y1) << 48);
}

// TEST register: alpha test, destination alpha test, depth test.
// ztest: 0 never, 1 always, 2 gequal, 3 greater.
constexpr uint64_t gs_test(bool alpha_enable, uint32_t alpha_method, uint8_t alpha_ref,
                           uint32_t alpha_fail, bool dest_alpha_enable, uint32_t dest_alpha_mode,
                           bool z_enable, uint32_t ztest)
{
    return static_cast<uint64_t>(alpha_enable) | (static_cast<uint64_t>(alpha_method) << 1) |
           (static_cast<uint64_t>(alpha_ref) << 4) | (static_cast<uint64_t>(alpha_fail) << 12) |
           (static_cast<uint64_t>(dest_alpha_enable) << 14) |
           (static_cast<uint64_t>(dest_alpha_mode) << 15) |
           (static_cast<uint64_t>(z_enable) << 16) | (static_cast<uint64_t>(ztest) << 17);
}

// ALPHA register: blend equation Cv = ((A - B) * C >> 7) + D, where the
// selectors are 0 = source, 1 = destination (frame), 2 = zero for A/B/D and
// 0 = source alpha, 1 = dest alpha, 2 = FIX for C.
// Standard alpha:  A=0 B=1 C=0 D=1  (Cs-Cd)*As + Cd
// Additive:        A=0 B=2 C=0 D=1  Cs*As + Cd
constexpr uint64_t gs_alpha(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint8_t fix = 0)
{
    return static_cast<uint64_t>(a) | (static_cast<uint64_t>(b) << 2) |
           (static_cast<uint64_t>(c) << 4) | (static_cast<uint64_t>(d) << 6) |
           (static_cast<uint64_t>(fix) << 32);
}

// FOGCOL register: the colour per-vertex F blends toward (F=255 no fog).
constexpr uint64_t gs_fogcol(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint64_t>(r) | (static_cast<uint64_t>(g) << 8) |
           (static_cast<uint64_t>(b) << 16);
}

constexpr uint64_t gs_bitbltbuf(uint32_t src_block, uint32_t src_width_units, PixelFormat src_fmt,
                                uint32_t dst_block, uint32_t dst_width_units, PixelFormat dst_fmt)
{
    return static_cast<uint64_t>(src_block) | (static_cast<uint64_t>(src_width_units) << 16) |
           (static_cast<uint64_t>(src_fmt) << 24) | (static_cast<uint64_t>(dst_block) << 32) |
           (static_cast<uint64_t>(dst_width_units) << 48) | (static_cast<uint64_t>(dst_fmt) << 56);
}

constexpr uint64_t gs_trxpos(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y,
                             uint32_t direction)
{
    return static_cast<uint64_t>(src_x) | (static_cast<uint64_t>(src_y) << 16) |
           (static_cast<uint64_t>(dst_x) << 32) | (static_cast<uint64_t>(dst_y) << 48) |
           (static_cast<uint64_t>(direction) << 59);
}

constexpr uint64_t gs_trxreg(uint32_t width, uint32_t height)
{
    return static_cast<uint64_t>(width) | (static_cast<uint64_t>(height) << 32);
}

// TEX0: texture base, buffer width, format, dimensions as log2, and how the
// texel combines with the vertex colour.
// tex_function: 0 MODULATE, 1 DECAL, 2 HIGHLIGHT, 3 HIGHLIGHT2.
constexpr uint64_t gs_tex0(uint32_t base_block, uint32_t width_units, PixelFormat fmt,
                           uint32_t log2_width, uint32_t log2_height, bool has_alpha,
                           uint32_t tex_function, uint32_t clut_block,
                           uint32_t clut_fmt, uint32_t clut_mode,
                           uint32_t clut_offset, uint32_t clut_load)
{
    return static_cast<uint64_t>(base_block & 0x3FFFu) |
           (static_cast<uint64_t>(width_units & 0x3Fu) << 14) |
           ((static_cast<uint64_t>(fmt) & 0x3Fu) << 20) |
           (static_cast<uint64_t>(log2_width & 0xFu) << 26) |
           (static_cast<uint64_t>(log2_height & 0xFu) << 30) |
           (static_cast<uint64_t>(has_alpha ? 1u : 0u) << 34) |
           (static_cast<uint64_t>(tex_function & 0x3u) << 35) |
           (static_cast<uint64_t>(clut_block & 0x3FFFu) << 37) |
           (static_cast<uint64_t>(clut_fmt & 0xFu) << 51) |
           (static_cast<uint64_t>(clut_mode & 0x1u) << 55) |
           (static_cast<uint64_t>(clut_offset & 0x1Fu) << 56) |
           (static_cast<uint64_t>(clut_load & 0x7u) << 61);
}

// TEX1: filtering. Nearest is the honest default on this hardware -- bilinear
// costs GS fill rate that a 30 fps budget cannot spare for most surfaces.
constexpr uint64_t gs_tex1_nearest()
{
    return 0; // LCM=0, MXL=0, MMAG=0 (NEAREST), MMIN=0 (NEAREST)
}

// TEXA: how alpha is expanded for formats that do not carry a full 8 bits.
constexpr uint64_t gs_texa(uint8_t alpha0, bool use_alpha_bit, uint8_t alpha1)
{
    return static_cast<uint64_t>(alpha0) |
           (static_cast<uint64_t>(use_alpha_bit ? 1u : 0u) << 15) |
           (static_cast<uint64_t>(alpha1) << 32);
}

// log2 for the power-of-two texture dimensions TEX0 requires.
constexpr uint32_t log2_pot(uint32_t v)
{
    uint32_t r = 0;
    while ((1u << r) < v) {
        ++r;
    }
    return r;
}

// ---- PACKED-mode vertex data ----------------------------------------------
//
// CRITICAL DISTINCTION. A GIF qword can carry a register value in two entirely
// different layouts, and they are not interchangeable:
//
//   A+D mode (begin_packed_ad / add_ad): the low 64 bits hold the register's
//   NATIVE value, exactly as the register is documented. The gs_frame /
//   gs_zbuf / gs_test / ... builders above produce these.
//
//   PACKED mode with an explicit register list (begin_packed): each field is
//   placed in its own 32-bit lane of the 128-bit qword, at fixed offsets that
//   have nothing to do with the register's native bit positions.
//
// Feeding a native value into a PACKED slot is silently accepted by the GS and
// rasterised as nonsense. It cost a bring-up session here: XYZ2's Y ended up
// reading the low half of the native Z field, so vertices landed at wild
// coordinates (huge distorted triangles) and RGBAQ's G read part of Q (colours
// came out dark). The full-screen clear sprite degenerated and drew nothing,
// which looked like "the clear is broken" rather than "the vertex layout is
// wrong".
//
// So: state registers go through add_ad(), vertex data goes through these.

// PACKED XYZ2/XYZ3: X in [15:0], Y in [47:32], Z in [95:64].
constexpr Qword gs_packed_xyz(uint32_t x, uint32_t y, uint32_t z)
{
    return Qword{static_cast<uint64_t>(x & 0xFFFFu) |
                     (static_cast<uint64_t>(y & 0xFFFFu) << 32),
                 static_cast<uint64_t>(z)};
}

// PACKED RGBAQ: R in [7:0], G in [39:32], B in [71:64], A in [103:96].
// Q is NOT carried here -- it comes from the ST register.
constexpr Qword gs_packed_rgbaq(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return Qword{static_cast<uint64_t>(r) | (static_cast<uint64_t>(g) << 32),
                 static_cast<uint64_t>(b) | (static_cast<uint64_t>(a) << 32)};
}

// PACKED ST: S in [31:0], T in [63:32], Q in [95:64] (all IEEE float bits).
constexpr Qword gs_packed_st(uint32_t s_bits, uint32_t t_bits, uint32_t q_bits)
{
    return Qword{static_cast<uint64_t>(s_bits) | (static_cast<uint64_t>(t_bits) << 32),
                 static_cast<uint64_t>(q_bits)};
}

// PACKED UV: U in [13:0], V in [45:32], both 12.4 fixed point.
constexpr Qword gs_packed_uv(uint32_t u, uint32_t v)
{
    return Qword{static_cast<uint64_t>(u & 0x3FFFu) |
                     (static_cast<uint64_t>(v & 0x3FFFu) << 32),
                 0};
}

// ---- Packet builder --------------------------------------------------------

class GsPacket {
public:
    // 'memory' must be 16-byte aligned and outlive the packet. Not owned.
    void init(Qword* memory, uint32_t capacity_qwords);

    void reset();

    Qword* data() { return m_base; }
    const Qword* data() const { return m_base; }
    uint32_t size() const { return m_count; }        // qwords written
    uint32_t capacity() const { return m_capacity; }
    uint32_t remaining() const { return m_capacity - m_count; }

    // Sticky: set when any write would have overrun. Check once before
    // sending rather than after every call -- a packet that overflowed must
    // never be handed to the DMAC.
    bool overflowed() const { return m_overflowed; }

    // Opens a PACKED-mode A+D block: 'nloop' register writes follow, each via
    // add_ad(). This is the general-purpose form used for state changes.
    void begin_packed_ad(uint32_t nloop, bool end_of_packet = false);

    // Opens a PACKED block with an explicit register layout, for vertex data
    // where the same register sequence repeats 'nloop' times.
    // 'regs' packs up to 16 GsReg values, 4 bits each, low-order first.
    void begin_packed(uint32_t nloop, uint32_t nreg, uint64_t regs,
                      bool end_of_packet = false, bool set_prim = false,
                      uint64_t prim = 0);

    // Opens an IMAGE-mode block: 'qwords' of raw pixel data follow, used for
    // texture upload (M2 task 6).
    void begin_image(uint32_t qwords, bool end_of_packet = false);

    // One A+D register write. Only valid inside a begin_packed_ad() block.
    void add_ad(GsReg reg, uint64_t value);

    // Raw qword, for vertex payloads and image data.
    void add_qword(uint64_t lo, uint64_t hi);
    void add_qword(const Qword& q) { add_qword(q.lo, q.hi); }

    // Appends a NOP-padded FINISH so the EE can wait for the GS to drain.
    void add_finish();

    // Retroactively sets the EOP bit on the most recently opened tag. Useful
    // when a packet is assembled in pieces and only the caller knows the end.
    void set_last_tag_eop();

private:
    Qword* write();

    Qword* m_base = nullptr;
    uint32_t m_capacity = 0;
    uint32_t m_count = 0;
    uint32_t m_last_tag = 0xFFFFFFFFu;
    bool m_overflowed = false;
};

// Packs a register list for begin_packed(): reg0 in the lowest nibble.
constexpr uint64_t gs_reglist(GsReg r0, GsReg r1 = GsReg::NOP, GsReg r2 = GsReg::NOP,
                              GsReg r3 = GsReg::NOP)
{
    return (static_cast<uint64_t>(r0) & 0xFu) | ((static_cast<uint64_t>(r1) & 0xFu) << 4) |
           ((static_cast<uint64_t>(r2) & 0xFu) << 8) | ((static_cast<uint64_t>(r3) & 0xFu) << 12);
}

} // namespace gfx
} // namespace ps2ur
