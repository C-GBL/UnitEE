#include "ps2ur/dma_chain.h"

#include "ps2ur/log.h"
#include "ps2ur/platform.h"
#include "ps2ur/vu_program.h"

#if defined(PS2UR_PLATFORM_PS2)
#include <dma.h>
#include <kernel.h>
#include <packet2.h>
#include <packet2_chain.h>
#include <packet2_utils.h>
#endif

namespace ps2ur {
namespace gfx {

namespace {
constexpr unsigned u32c(uint32_t v) { return static_cast<unsigned>(v); }
} // namespace

#if defined(PS2UR_PLATFORM_PS2)

bool DmaChain::init(uint32_t max_qwords)
{
    shutdown();
    packet2_t* p = packet2_create(static_cast<u16>(max_qwords), P2_TYPE_NORMAL,
                                  P2_MODE_CHAIN, 1);
    if (p == nullptr) {
        log(LogLevel::Error, "chain: could not allocate %u qwords", u32c(max_qwords));
        return false;
    }
    m_packet = p;
    m_capacity = max_qwords;
    return true;
}

void DmaChain::shutdown()
{
    if (m_packet != nullptr) {
        wait();
        packet2_free(static_cast<packet2_t*>(m_packet));
        m_packet = nullptr;
    }
}

void DmaChain::begin()
{
    if (m_in_flight) {
        wait();
    }
    packet2_reset(static_cast<packet2_t*>(m_packet), 0);
    m_open = true;
    m_stats.batches = 0;
    m_stats.qwords = 0;
    m_stats.build_ticks = 0;
    m_stats.kick_ticks = 0;
    m_stats.wait_ticks = 0;
}

bool DmaChain::add_constants(const void* qwords, uint32_t count, uint32_t dest)
{
    if (!m_open || qwords == nullptr || count == 0) {
        return false;
    }
    const uint64_t t0 = platform::now_ticks();
    packet2_t* p = static_cast<packet2_t*>(m_packet);
    packet2_utils_vu_open_unpack(p, dest, 0);
    packet2_add_data(p, const_cast<void*>(qwords), count);
    packet2_utils_vu_close_unpack(p);
    m_stats.qwords += count;
    m_stats.build_ticks += platform::now_ticks() - t0;
    return true;
}

bool DmaChain::add_batch(const BatchBlock& block)
{
    if (!m_open || block.header == nullptr || block.verts == nullptr) {
        return false;
    }
    const uint64_t t0 = platform::now_ticks();
    packet2_t* p = static_cast<packet2_t*>(m_packet);
    // The SDK-proven per-batch sequence: two ref-tag unpacks (the VIF codes
    // ride in the tag's upper 64 bits via TTE), then FLUSH + MSCAL 0 as a
    // small cnt segment.
    packet2_utils_vu_add_unpack_data(p, 7, const_cast<Qword*>(block.header), 2, 0);
    packet2_utils_vu_add_unpack_data(p, block.vert_dest,
                                     const_cast<Qword*>(block.verts),
                                     block.vert_qwords, 0);
    packet2_utils_vu_add_start_program(p, 0);
    m_stats.batches++;
    m_stats.qwords += 2u + block.vert_qwords;
    m_stats.build_ticks += platform::now_ticks() - t0;
    return true;
}

bool DmaChain::kick()
{
    if (!m_open) {
        return false;
    }
    packet2_t* p = static_cast<packet2_t*>(m_packet);

    // packet2 asserts internally on overflow; belt-and-braces check here so a
    // full chain drops the frame loudly instead of running past the buffer.
    const uint32_t used = static_cast<uint32_t>(packet2_get_qw_count(p));
    if (used + 2u > m_capacity) {
        log(LogLevel::Error, "chain: %u qwords exceeds capacity %u; frame dropped",
            u32c(used), u32c(m_capacity));
        m_stats.overflows++;
        m_open = false;
        return false;
    }

    packet2_utils_vu_add_end_tag(p);
    const uint64_t t0 = platform::now_ticks();
    // Flush the EE cache over the chain AND every ref'd block: the blocks were
    // written through the cache and the DMAC reads physical RAM (the same
    // lesson the GIF path learned at M2).
    FlushCache(0);
    dma_channel_send_packet2(p, DMA_CHANNEL_VIF1, 1);
    m_stats.kick_ticks += platform::now_ticks() - t0;
    m_open = false;
    m_in_flight = true;
    return true;
}

void DmaChain::wait()
{
    if (!m_in_flight) {
        return;
    }
    const uint64_t t0 = platform::now_ticks();
    dma_channel_wait(DMA_CHANNEL_VIF1, 0);
    vu::wait_idle();
    m_stats.wait_ticks += platform::now_ticks() - t0;
    m_in_flight = false;
}

#else // host build: structural checks only, no hardware

bool DmaChain::init(uint32_t max_qwords)
{
    m_capacity = max_qwords;
    return max_qwords > 0;
}

void DmaChain::shutdown()
{
    m_capacity = 0;
}

void DmaChain::begin()
{
    m_open = true;
    m_stats.batches = 0;
    m_stats.qwords = 0;
}

bool DmaChain::add_constants(const void* qwords, uint32_t count, uint32_t dest)
{
    (void)dest;
    if (!m_open || qwords == nullptr || count == 0) {
        return false;
    }
    m_stats.qwords += count;
    return m_stats.qwords <= m_capacity;
}

bool DmaChain::add_batch(const BatchBlock& block)
{
    if (!m_open || block.header == nullptr || block.verts == nullptr) {
        return false;
    }
    m_stats.batches++;
    m_stats.qwords += 2u + block.vert_qwords;
    return true;
}

bool DmaChain::kick()
{
    if (!m_open) {
        return false;
    }
    m_open = false;
    return true;
}

void DmaChain::wait() {}

#endif

} // namespace gfx
} // namespace ps2ur
