#include "ps2ur/gs_packet.h"

#include "ps2ur/assert.h"

namespace ps2ur {
namespace gfx {

namespace {

// GIFtag layout (128 bits):
//   [14:0]   NLOOP
//   [15]     EOP
//   [45:16]  reserved
//   [46]     PRE   (apply PRIM field)
//   [57:47]  PRIM
//   [59:58]  FLG   (0 PACKED, 1 REGLIST, 2 IMAGE, 3 disable)
//   [63:60]  NREG  (0 means 16)
//   [127:64] REGS  (4 bits per register)
constexpr uint64_t gif_tag_lo(uint32_t nloop, bool eop, bool pre, uint64_t prim,
                              GifMode flg, uint32_t nreg)
{
    return (static_cast<uint64_t>(nloop) & 0x7FFFu) | (static_cast<uint64_t>(eop) << 15) |
           (static_cast<uint64_t>(pre) << 46) | ((prim & 0x7FFu) << 47) |
           (static_cast<uint64_t>(flg) << 58) | (static_cast<uint64_t>(nreg & 0xFu) << 60);
}

} // namespace

void GsPacket::init(Qword* memory, uint32_t capacity_qwords)
{
    PS2UR_ASSERT(memory != nullptr || capacity_qwords == 0);
    // The DMAC transfers qwords; a misaligned packet base corrupts everything
    // downstream in ways that are painful to diagnose on hardware.
    PS2UR_ASSERT((reinterpret_cast<uintptr_t>(memory) & 15u) == 0);
    m_base = memory;
    m_capacity = capacity_qwords;
    reset();
}

void GsPacket::reset()
{
    m_count = 0;
    m_last_tag = 0xFFFFFFFFu;
    m_overflowed = false;
}

Qword* GsPacket::write()
{
    if (m_count >= m_capacity) {
        m_overflowed = true;
        return nullptr;
    }
    return &m_base[m_count++];
}

void GsPacket::begin_packed_ad(uint32_t nloop, bool end_of_packet)
{
    begin_packed(nloop, 1, kGifRegAD, end_of_packet);
}

void GsPacket::begin_packed(uint32_t nloop, uint32_t nreg, uint64_t regs,
                            bool end_of_packet, bool set_prim, uint64_t prim)
{
    Qword* q = write();
    if (q == nullptr) {
        return;
    }
    m_last_tag = m_count - 1;
    q->lo = gif_tag_lo(nloop, end_of_packet, set_prim, prim, GifMode::Packed, nreg);
    q->hi = regs;
}

void GsPacket::begin_image(uint32_t qwords, bool end_of_packet)
{
    Qword* q = write();
    if (q == nullptr) {
        return;
    }
    m_last_tag = m_count - 1;
    // IMAGE mode ignores REGS and NREG; NLOOP counts the raw data qwords.
    q->lo = gif_tag_lo(qwords, end_of_packet, false, 0, GifMode::Image, 0);
    q->hi = 0;
}

void GsPacket::add_ad(GsReg reg, uint64_t value)
{
    Qword* q = write();
    if (q == nullptr) {
        return;
    }
    // A+D format: data in the low 64 bits, register address in the high half.
    q->lo = value;
    q->hi = static_cast<uint64_t>(reg);
}

void GsPacket::add_qword(uint64_t lo, uint64_t hi)
{
    Qword* q = write();
    if (q == nullptr) {
        return;
    }
    q->lo = lo;
    q->hi = hi;
}

void GsPacket::add_finish()
{
    // FINISH raises the GS finish event once every preceding primitive has
    // been drawn; the EE waits on it rather than guessing at timing.
    begin_packed_ad(1);
    add_ad(GsReg::FINISH, 0);
}

void GsPacket::set_last_tag_eop()
{
    if (m_last_tag < m_count) {
        m_base[m_last_tag].lo |= (1ull << 15);
    }
}

static_assert(kGifRegAD == 0x0E, "A+D descriptor must be 0x0E (ps2sdk GIF_REG_AD)");

} // namespace gfx
} // namespace ps2ur
