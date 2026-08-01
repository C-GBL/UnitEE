#include "ps2ur/vu_program.h"

#include "ps2ur/assert.h"
#include "ps2ur/log.h"

#if defined(PS2UR_PLATFORM_PS2)
#include <dma.h>
#include <dma_tags.h>
#include <kernel.h>
#include <packet2.h>
#include <packet2_utils.h>
#endif

namespace ps2ur {
namespace vu {

namespace {
constexpr unsigned u(uint32_t v) { return static_cast<unsigned>(v); }
} // namespace

void MicroProgram::set_blob(const void* start, const void* end, uint32_t vu_address)
{
    PS2UR_ASSERT(start != nullptr && end != nullptr);
    m_start = start;
    m_end = end;
    m_vu_address = vu_address;
    // dvp-as emits 64-bit instruction pairs, so the blob length in bytes
    // divided by 8 is the instruction count.
    const uint8_t* s = static_cast<const uint8_t*>(start);
    const uint8_t* e = static_cast<const uint8_t*>(end);
    m_size_instructions = static_cast<uint32_t>((e - s) / 8);
    m_uploaded = false;

    if (m_vu_address + m_size_instructions > kMicroMemInstructions) {
        log(LogLevel::Error,
            "vu: program of %u instructions at %u overflows micro memory (%u)",
            u(m_size_instructions), u(m_vu_address), u(kMicroMemInstructions));
    }
}

#if defined(PS2UR_PLATFORM_PS2)

bool MicroProgram::upload()
{
    if (m_start == nullptr || m_size_instructions == 0) {
        return false;
    }

    u32* start = reinterpret_cast<u32*>(const_cast<void*>(m_start));
    u32* end = reinterpret_cast<u32*>(const_cast<void*>(m_end));

    // The helper reports how many qwords the MPG chain will take, so the
    // packet is sized exactly rather than guessed at.
    const u32 qwords = packet2_utils_get_packet_size_for_program(start, end) + 1u;
    packet2_t* packet = packet2_create(static_cast<u16>(qwords), P2_TYPE_NORMAL,
                                       P2_MODE_CHAIN, 1);
    if (packet == nullptr) {
        log(LogLevel::Error, "vu: could not allocate a %u-qword MPG packet", u(qwords));
        return false;
    }

    packet2_vif_add_micro_program(packet, m_vu_address, start, end);
    packet2_utils_vu_add_end_tag(packet);
    dma_channel_send_packet2(packet, DMA_CHANNEL_VIF1, 1);
    dma_channel_wait(DMA_CHANNEL_VIF1, 0);
    packet2_free(packet);

    m_uploaded = true;
    log(LogLevel::Info, "vu: uploaded %u instructions to micro memory at %u",
        u(m_size_instructions), u(m_vu_address));
    return true;
}

bool MicroProgram::unpack_data(const void* qwords, uint32_t count, uint32_t dest_qword)
{
    if (qwords == nullptr || count == 0) {
        return false;
    }
    if (dest_qword + count > kDataMemQwords) {
        log(LogLevel::Error, "vu: unpack of %u qwords at %u overflows data memory",
            u(count), u(dest_qword));
        return false;
    }

    packet2_t* packet = packet2_create(static_cast<u16>(count + 8u), P2_TYPE_NORMAL,
                                       P2_MODE_CHAIN, 1);
    if (packet == nullptr) {
        return false;
    }

    // use_top = 0: address is absolute rather than relative to the double
    // buffer base. Double buffering arrives with the real batch pipeline.
    packet2_utils_vu_open_unpack(packet, dest_qword, 0);
    packet2_add_data(packet, const_cast<void*>(qwords), count);
    packet2_utils_vu_close_unpack(packet);
    packet2_utils_vu_add_end_tag(packet);

    dma_channel_send_packet2(packet, DMA_CHANNEL_VIF1, 1);
    dma_channel_wait(DMA_CHANNEL_VIF1, 0);
    packet2_free(packet);
    return true;
}

bool MicroProgram::start(uint32_t entry_offset)
{
    if (!m_uploaded) {
        log(LogLevel::Error, "vu: start() before upload()");
        return false;
    }
    packet2_t* packet = packet2_create(8, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
    if (packet == nullptr) {
        return false;
    }
    packet2_utils_vu_add_start_program(packet, m_vu_address + entry_offset);
    packet2_utils_vu_add_end_tag(packet);
    dma_channel_send_packet2(packet, DMA_CHANNEL_VIF1, 1);
    dma_channel_wait(DMA_CHANNEL_VIF1, 0);
    packet2_free(packet);
    return true;
}


bool MicroProgram::init_batching(uint32_t max_data_qwords)
{
    shutdown_batching();
    // Header room for the VIF codes around the payload: an UNPACK open/close
    // pair and the MSCAL, plus the chain end tag.
    const uint32_t qwords = max_data_qwords + 16u;
    packet2_t* p = packet2_create(static_cast<u16>(qwords), P2_TYPE_NORMAL,
                                  P2_MODE_CHAIN, 1);
    if (p == nullptr) {
        log(LogLevel::Error, "vu: could not allocate a %u-qword batch packet", u(qwords));
        return false;
    }
    m_batch_packet = p;
    m_batch_capacity = max_data_qwords;
    return true;
}

void MicroProgram::shutdown_batching()
{
    if (m_batch_packet != nullptr) {
        packet2_free(static_cast<packet2_t*>(m_batch_packet));
        m_batch_packet = nullptr;
        m_batch_capacity = 0;
    }
}

bool MicroProgram::draw_batch(const void* qwords, uint32_t count, uint32_t dest_qword,
                              uint32_t entry_offset)
{
    if (!m_uploaded || m_batch_packet == nullptr || qwords == nullptr || count == 0) {
        return false;
    }
    if (count > m_batch_capacity || dest_qword + count > kDataMemQwords) {
        log(LogLevel::Error, "vu: batch of %u qwords exceeds capacity", u(count));
        return false;
    }

    packet2_t* p = static_cast<packet2_t*>(m_batch_packet);
    packet2_reset(p, 0);

    packet2_utils_vu_open_unpack(p, dest_qword, 0);
    packet2_add_data(p, const_cast<void*>(qwords), count);
    packet2_utils_vu_close_unpack(p);
    packet2_utils_vu_add_start_program(p, m_vu_address + entry_offset);
    packet2_utils_vu_add_end_tag(p);

    // Wait for the PREVIOUS transfer before overwriting the packet, not after
    // this one: that keeps the EE building batch N+1 while the DMAC walks N.
    dma_channel_wait(DMA_CHANNEL_VIF1, 0);
    dma_channel_send_packet2(p, DMA_CHANNEL_VIF1, 1);
    return true;
}

void MicroProgram::wait_batches()
{
    dma_channel_wait(DMA_CHANNEL_VIF1, 0);
    wait_idle();
}

void wait_idle()
{
    // VU1 busy is VIF1_STAT bit 2 (VEW: waiting on the VU). Bounded so a
    // microprogram that never terminates cannot hang the whole run -- a VU
    // program missing its E bit is a very easy mistake to make.
    volatile uint32_t* const vif1_stat = reinterpret_cast<volatile uint32_t*>(0x10003C00);
    uint32_t spins = 0;
    while ((*vif1_stat & (1u << 2)) != 0) {
        if (++spins > 50000000u) {
            log(LogLevel::Error, "vu: wait_idle timed out -- microprogram never ended "
                                 "(missing the E bit?)");
            return;
        }
    }
}

#else // host build

bool MicroProgram::upload()
{
    m_uploaded = m_start != nullptr && m_size_instructions != 0;
    return m_uploaded;
}

bool MicroProgram::unpack_data(const void* qwords, uint32_t count, uint32_t dest_qword)
{
    // Bounds are checked on both platforms so the host tests catch an
    // over-large unpack without a console.
    if (qwords == nullptr || count == 0) {
        return false;
    }
    return dest_qword + count <= kDataMemQwords;
}

bool MicroProgram::start(uint32_t entry_offset)
{
    (void)entry_offset;
    return m_uploaded;
}

bool MicroProgram::init_batching(uint32_t max_data_qwords)
{
    m_batch_capacity = max_data_qwords;
    return true;
}

void MicroProgram::shutdown_batching()
{
    m_batch_capacity = 0;
}

bool MicroProgram::draw_batch(const void* qwords, uint32_t count, uint32_t dest_qword,
                              uint32_t entry_offset)
{
    (void)entry_offset;
    if (qwords == nullptr || count == 0) {
        return false;
    }
    return count <= m_batch_capacity && dest_qword + count <= kDataMemQwords;
}

void MicroProgram::wait_batches() {}

void wait_idle() {}

#endif

} // namespace vu
} // namespace ps2ur
