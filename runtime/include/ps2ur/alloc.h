// Allocators (plan section 8: src/core -- "allocators, time, log, assert, scratchpad").
// The PS2 has 32 MB total (plan section 3.1); allocation is budgeted, not open-ended.
// v1: a linear/arena allocator plus a heap facade. Scratchpad (SPR) allocator comes
// later with the PS2 platform layer.
#pragma once

#include <cstddef>
#include <cstdint>

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

// LIFO allocator over caller-owned memory. Same bump-pointer cost as Arena,
// but a marker can release everything allocated after it -- the natural fit
// for per-frame and per-pass scratch, where the lifetime really is a stack.
class StackAllocator {
public:
    using Marker = size_t;

    void init(void* memory, size_t size);

    // Returns nullptr on exhaustion. 'align' must be a power of two.
    void* alloc(size_t size, size_t align = 16);

    // Current top of stack; pass to rewind() to free everything after it.
    Marker marker() const { return m_offset; }

    // Releases everything allocated since 'm'. Rewinding to a marker taken
    // before an earlier rewind is allowed (it just frees more).
    void rewind(Marker m);

    void reset();
    size_t remaining() const;
    const ArenaStats& stats() const { return m_stats; }

private:
    unsigned char* m_base = nullptr;
    size_t m_size = 0;
    size_t m_offset = 0;
    ArenaStats m_stats;
};

// Fixed-size block allocator. O(1) alloc and free, no fragmentation, which is
// what a 32 MB machine with no virtual memory needs for anything churning
// (particles, packets, component records). The free list is threaded through
// the free blocks themselves, so there is no side table.
class PoolAllocator {
public:
    // Carves 'memory' into blocks of 'block_size' (rounded up to 'align').
    // block_size must be >= sizeof(void*) after rounding.
    void init(void* memory, size_t size, size_t block_size, size_t align = 16);

    void* alloc();          // nullptr when exhausted
    void free(void* block); // nullptr is a no-op

    size_t block_size() const { return m_block_size; }
    size_t capacity() const { return m_block_count; }   // total blocks
    size_t used() const { return m_used; }              // blocks handed out
    const ArenaStats& stats() const { return m_stats; }

private:
    unsigned char* m_base = nullptr;
    void* m_free_list = nullptr;
    size_t m_block_size = 0;
    size_t m_block_count = 0;
    size_t m_used = 0;
    ArenaStats m_stats;
};

// Scratchpad RAM (plan section 3.1: 16 KB at 0x70000000, single-cycle,
// DMA-addressable -- "the single most valuable resource on the machine").
// Used for DMA chain assembly and hot working sets.
//
// On the host build this is an ordinary heap block of the same size, so code
// that budgets against it can be tested off-target. Anything that depends on
// the real address (DMA) must go through the platform layer, not here.
//
// IMPORTANT (plan section 11.4): the scratchpad must be excluded from
// conservative GC scanning, and a GC-visible pointer must never live only
// here.
namespace scratchpad {

size_t size();      // 16384
void* base();       // nullptr before init()
Arena& arena();     // arena covering the whole scratchpad

void init();
void shutdown();

} // namespace scratchpad

// Heap facade. Every allocation carries its size in a header, so the facade
// can answer the two questions D4 asks (plan section 2: peak RAM <= 30 MB of
// the 32, no allocation failure across a 30-minute soak): how much is out
// right now, and how much has ever been out at once.
struct HeapStats {
    size_t outstanding = 0; // bytes currently handed out (payload only)
    size_t peak = 0;        // high-water mark of outstanding
    size_t budget = 0;      // ceiling; 0 means unlimited
    uint32_t allocs = 0;
    uint32_t frees = 0;
    uint32_t failures = 0; // refused by the budget, or the backing heap said no
};

// Sets the ceiling in bytes. An allocation that would cross it fails (returns
// nullptr) and increments failures, rather than being allowed through and
// discovered later as a crash on a machine with no virtual memory. 0 removes
// the ceiling. Lowering it below what is already out is allowed: nothing is
// reclaimed, but further growth stops.
void heap_set_budget(size_t bytes);

const HeapStats& heap_stats();

// Zeroes the counters. Does not touch live allocations; outstanding is
// preserved because forgetting it would make the next free underflow.
void heap_reset_stats();

void* heap_alloc(size_t size, size_t align = 16);
void heap_free(void* ptr);

} // namespace ps2ur
