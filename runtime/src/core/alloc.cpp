#include "ps2ur/alloc.h"

#include "ps2ur/assert.h"

#include <cstdint>
#include <cstdlib>

namespace ps2ur {

static bool is_pow2(size_t v) { return v != 0 && (v & (v - 1)) == 0; }

// ---- Arena -----------------------------------------------------------------

void Arena::init(void* memory, size_t size)
{
    PS2UR_ASSERT(memory != nullptr || size == 0);
    m_base = static_cast<unsigned char*>(memory);
    m_size = size;
    m_offset = 0;
    m_stats = ArenaStats{};
    m_stats.capacity = size;
}

void* Arena::alloc(size_t size, size_t align)
{
    PS2UR_ASSERT(m_base != nullptr);
    PS2UR_ASSERT(is_pow2(align));

    const size_t aligned = (m_offset + (align - 1)) & ~(align - 1);
    if (aligned > m_size || size > m_size - aligned) {
        m_stats.fail_count++;
        return nullptr; // exhausted -- caller decides; no exceptions in runtime code
    }
    m_offset = aligned + size;
    m_stats.used = m_offset;
    if (m_offset > m_stats.peak) {
        m_stats.peak = m_offset;
    }
    m_stats.alloc_count++;
    return m_base + aligned;
}

void Arena::reset()
{
    m_offset = 0;
    m_stats.used = 0;
    // peak / alloc_count / fail_count deliberately survive reset for D4-style
    // soak instrumentation (plan section 2).
}

size_t Arena::remaining() const
{
    return m_size - m_offset;
}

// ---- StackAllocator --------------------------------------------------------

void StackAllocator::init(void* memory, size_t size)
{
    PS2UR_ASSERT(memory != nullptr || size == 0);
    m_base = static_cast<unsigned char*>(memory);
    m_size = size;
    m_offset = 0;
    m_stats = ArenaStats{};
    m_stats.capacity = size;
}

void* StackAllocator::alloc(size_t size, size_t align)
{
    PS2UR_ASSERT(m_base != nullptr);
    PS2UR_ASSERT(is_pow2(align));

    const size_t aligned = (m_offset + (align - 1)) & ~(align - 1);
    if (aligned > m_size || size > m_size - aligned) {
        m_stats.fail_count++;
        return nullptr;
    }
    m_offset = aligned + size;
    m_stats.used = m_offset;
    if (m_offset > m_stats.peak) {
        m_stats.peak = m_offset;
    }
    m_stats.alloc_count++;
    return m_base + aligned;
}

void StackAllocator::rewind(Marker m)
{
    // Rewinding forward would hand out memory that was never allocated.
    PS2UR_ASSERT(m <= m_offset);
    if (m > m_offset) {
        return;
    }
    m_offset = m;
    m_stats.used = m_offset;
}

void StackAllocator::reset()
{
    m_offset = 0;
    m_stats.used = 0;
    // peak/alloc_count/fail_count survive reset, as with Arena.
}

size_t StackAllocator::remaining() const
{
    return m_size - m_offset;
}

// ---- PoolAllocator ---------------------------------------------------------

void PoolAllocator::init(void* memory, size_t size, size_t block_size, size_t align)
{
    PS2UR_ASSERT(memory != nullptr || size == 0);
    PS2UR_ASSERT(is_pow2(align));

    // Every block must be able to hold the free-list link while free.
    size_t stride = (block_size + (align - 1)) & ~(align - 1);
    if (stride < sizeof(void*)) {
        stride = sizeof(void*);
    }

    unsigned char* base = static_cast<unsigned char*>(memory);
    const uintptr_t aligned_base =
        (reinterpret_cast<uintptr_t>(base) + (align - 1)) & ~static_cast<uintptr_t>(align - 1);
    const size_t lost = static_cast<size_t>(aligned_base - reinterpret_cast<uintptr_t>(base));

    m_base = reinterpret_cast<unsigned char*>(aligned_base);
    m_block_size = stride;
    m_block_count = (size > lost) ? (size - lost) / stride : 0;
    m_used = 0;
    m_stats = ArenaStats{};
    m_stats.capacity = m_block_count * stride;

    // Thread the free list through the blocks, in order, so the first
    // allocations walk memory forwards and stay cache-friendly.
    m_free_list = nullptr;
    for (size_t i = m_block_count; i > 0; --i) {
        void* block = m_base + (i - 1) * stride;
        *reinterpret_cast<void**>(block) = m_free_list;
        m_free_list = block;
    }
}

void* PoolAllocator::alloc()
{
    if (m_free_list == nullptr) {
        m_stats.fail_count++;
        return nullptr;
    }
    void* block = m_free_list;
    m_free_list = *reinterpret_cast<void**>(block);
    m_used++;
    m_stats.used = m_used * m_block_size;
    if (m_stats.used > m_stats.peak) {
        m_stats.peak = m_stats.used;
    }
    m_stats.alloc_count++;
    return block;
}

void PoolAllocator::free(void* block)
{
    if (block == nullptr) {
        return;
    }
    // Catch a pointer that did not come from this pool, and a misaligned one
    // (which would corrupt the free list silently).
    PS2UR_ASSERT(block >= m_base);
    PS2UR_ASSERT(block < m_base + m_block_count * m_block_size);
    PS2UR_ASSERT((static_cast<size_t>(static_cast<unsigned char*>(block) - m_base) %
                  m_block_size) == 0);

    *reinterpret_cast<void**>(block) = m_free_list;
    m_free_list = block;
    PS2UR_ASSERT(m_used > 0);
    m_used--;
    m_stats.used = m_used * m_block_size;
}

// ---- Scratchpad ------------------------------------------------------------

namespace scratchpad {

namespace {
void* g_base = nullptr;
Arena g_arena;
#if !defined(PS2UR_PLATFORM_PS2)
void* g_host_block = nullptr;
#endif
} // namespace

size_t size() { return 16 * 1024; }
void* base() { return g_base; }
Arena& arena() { return g_arena; }

void init()
{
    if (g_base != nullptr) {
        return;
    }
#if defined(PS2UR_PLATFORM_PS2)
    // The EE scratchpad is a fixed hardware region, not an allocation
    // (plan section 3.1).
    g_base = reinterpret_cast<void*>(0x70000000u);
#else
    g_host_block = heap_alloc(size(), 16);
    g_base = g_host_block;
#endif
    g_arena.init(g_base, g_base != nullptr ? size() : 0);
}

void shutdown()
{
#if !defined(PS2UR_PLATFORM_PS2)
    heap_free(g_host_block);
    g_host_block = nullptr;
#endif
    g_base = nullptr;
    g_arena.init(nullptr, 0);
}

} // namespace scratchpad

// ---- Heap facade -----------------------------------------------------------
// malloc-backed with manual alignment, plus the accounting D4 needs (plan
// section 2, M13 task 2). Two pointer-sized slots sit immediately below every
// user pointer: the raw block to hand back to free, and the payload size.
// Keeping the size means the facade can report outstanding bytes and a
// high-water mark without a side table, which on a machine with no virtual
// memory is the difference between "we fit" and "we think we fit".

namespace {

HeapStats g_heap;
constexpr size_t kHeapHeader = 2 * sizeof(void*);

} // namespace

void heap_set_budget(size_t bytes) { g_heap.budget = bytes; }

const HeapStats& heap_stats() { return g_heap; }

void heap_reset_stats()
{
    const size_t outstanding = g_heap.outstanding;
    const size_t budget = g_heap.budget;
    g_heap = HeapStats{};
    // Outstanding is live state, not a statistic: zeroing it would make the
    // next free underflow the counter and report nonsense forever after.
    g_heap.outstanding = outstanding;
    g_heap.peak = outstanding;
    g_heap.budget = budget;
}

void* heap_alloc(size_t size, size_t align)
{
    PS2UR_ASSERT(is_pow2(align));
    if (align < kHeapHeader) {
        align = kHeapHeader;
    }

    // Refuse before allocating rather than after: the point of a budget on
    // this machine is that the failure is visible here, at a named call site,
    // instead of as an out-of-memory death somewhere unrelated later.
    if (g_heap.budget != 0 && g_heap.outstanding + size > g_heap.budget) {
        g_heap.failures++;
        return nullptr;
    }

    void* raw = std::malloc(size + align + kHeapHeader);
    if (raw == nullptr) {
        g_heap.failures++;
        return nullptr;
    }
    const uintptr_t user =
        (reinterpret_cast<uintptr_t>(raw) + kHeapHeader + (align - 1)) &
        ~static_cast<uintptr_t>(align - 1);
    reinterpret_cast<void**>(user)[-1] = raw;
    reinterpret_cast<size_t*>(reinterpret_cast<void**>(user) - 2)[0] = size;

    g_heap.allocs++;
    g_heap.outstanding += size;
    if (g_heap.outstanding > g_heap.peak) {
        g_heap.peak = g_heap.outstanding;
    }
    return reinterpret_cast<void*>(user);
}

void heap_free(void* ptr)
{
    if (ptr == nullptr) {
        return;
    }
    void** user = static_cast<void**>(ptr);
    const size_t size = reinterpret_cast<size_t*>(user - 2)[0];
    void* raw = user[-1];

    g_heap.frees++;
    g_heap.outstanding = size > g_heap.outstanding ? 0 : g_heap.outstanding - size;
    std::free(raw);
}

} // namespace ps2ur
