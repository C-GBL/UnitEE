// Per-frame VIF1 DMA chain (plan section 9, M4 tasks 5 and 7).
//
// The frame's whole VU1 workload is described once and kicked with a single
// dma_channel_send: a cnt segment unpacks the per-mesh constants, then one
// `ref` tag per batch points at its self-contained block in main RAM (see
// gs_batch.h). The DMAC walks the chain and the VIF serialises MSCALs against
// the running microprogram, all without the EE.
//
// This replaces MicroProgram::draw_batch (one transfer per batch) as the
// shipping draw path; draw_batch stays for bring-up and tests.
//
// Deviation from the plan, recorded: section 9 M4 task 5 says to assemble the
// chain in SCRATCHPAD. This version assembles it in main RAM. The scratchpad
// is the right home once chain assembly shows up in a profile; the profiling
// counters this class carries (task 7) are exactly how that decision will be
// made, and the throughput measurement is currently vsync-locked with the
// chain in main RAM.
#pragma once

#include "ps2ur/gs_batch.h"
#include "ps2ur/gs_packet.h"

#include <cstdint>

namespace ps2ur {
namespace gfx {

class DmaChain {
public:
    struct Stats {
        uint32_t batches = 0;       // ref tags this frame
        uint32_t qwords = 0;        // total qwords referenced + inlined
        uint64_t build_ticks = 0;   // EE time spent appending to the chain
        uint64_t kick_ticks = 0;    // EE time in the send call
        uint64_t wait_ticks = 0;    // EE time blocked waiting for completion
        uint32_t overflows = 0;     // frames dropped because the chain filled
    };

    bool init(uint32_t max_qwords);
    void shutdown();

    // Starts a new frame's chain. Waits for the previous kick if it is still
    // in flight, so chain memory is never rewritten under the DMAC.
    void begin();

    // Inline (cnt) unpack of 'count' qwords to VU data address 'dest' --
    // used for per-mesh constants.
    bool add_constants(const void* qwords, uint32_t count, uint32_t dest);

    // One batch: ref-tag unpacks for the block's header (tag+count -> VU 7)
    // and vertices (-> block.vert_dest), then FLUSH + MSCAL 0. Three chain
    // tags per batch, all pointing at data that never moves -- the pattern
    // the SDK ships and its own samples prove.
    bool add_batch(const BatchBlock& block, uint32_t mscal_addr = 0);

    // Ends the chain and starts the DMA. Returns immediately; the EE can go
    // build the next frame.
    bool kick();

    // Blocks until the chain has fully drained and VU1 is idle.
    void wait();

    const Stats& stats() const { return m_stats; }
    void reset_stats() { m_stats = Stats{}; }

private:
    void* m_packet = nullptr; // packet2_t*, opaque outside the ps2 build
    uint32_t m_capacity = 0;
    bool m_open = false;
    bool m_in_flight = false;
    Stats m_stats;
};

} // namespace gfx
} // namespace ps2ur
