// Allocators (plan section 8: src/core -- "allocators, time, log, assert, scratchpad").
// The PS2 has 32 MB total (plan section 3.1); allocation is budgeted, not open-ended.
// v1: a linear/arena allocator plus a heap facade. Scratchpad (SPR) allocator comes
// later with the PS2 platform layer.
#pragma once

#include <cstddef>

namespace ps2ur {

struct ArenaStats {
    size_t capacity = 0;    // bytes managed
    size_t used = 0;        // bytes currently allocated (including align padding)
    size_t peak = 0;        // high-water mark of used
    size_t alloc_count = 0; // successful allocations since init
    size_t fail_count = 0;  // failed (exhausted) allocations since init
};

// Linear (bump) allocator over caller-owned memory. No per-allocation free;
// reset() releases everything at once. Not thread-safe (the EE side is
// single-threaded by design).
class Arena {
public:
    // 'memory' must outlive the arena. Does not take ownership.
    void init(void* memory, size_t size);

    // Returns nullptr on exhaustion. 'align' must be a power of two.
    // Default 16: qword alignment is the natural unit on the EE (128-bit regs, DMA).
    void* alloc(size_t size, size_t align = 16);

    // Releases all allocations. Keeps peak/alloc_count/fail_count statistics.
    void reset();

    size_t remaining() const;
    const ArenaStats& stats() const { return m_stats; }

private:
    unsigned char* m_base = nullptr;
    size_t m_size = 0;
    size_t m_offset = 0;
    ArenaStats m_stats;
};

// Heap facade. Host: malloc-backed. PS2: will carve from a fixed budget so D4
// (peak RAM <= 30 MB, plan section 2) is enforceable.
// TODO(spec missing: section 9): per-milestone memory budgets and heap layout.
void* heap_alloc(size_t size, size_t align = 16);
void heap_free(void* ptr);

} // namespace ps2ur
