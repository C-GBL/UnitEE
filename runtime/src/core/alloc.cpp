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

// ---- Heap facade -----------------------------------------------------------
// Host: malloc-backed with manual alignment. PS2: to be replaced by a
// fixed-budget allocator. TODO(spec missing: section 9): heap budget/layout.

void* heap_alloc(size_t size, size_t align)
{
    PS2UR_ASSERT(is_pow2(align));
    if (align < sizeof(void*)) {
        align = sizeof(void*);
    }
    void* raw = std::malloc(size + align + sizeof(void*));
    if (raw == nullptr) {
        return nullptr;
    }
    const uintptr_t user =
        (reinterpret_cast<uintptr_t>(raw) + sizeof(void*) + (align - 1)) &
        ~static_cast<uintptr_t>(align - 1);
    reinterpret_cast<void**>(user)[-1] = raw;
    return reinterpret_cast<void*>(user);
}

void heap_free(void* ptr)
{
    if (ptr != nullptr) {
        std::free(static_cast<void**>(ptr)[-1]);
    }
}

} // namespace ps2ur
