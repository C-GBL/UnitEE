// StackAllocator, PoolAllocator, and scratchpad unit tests (plan section 9,
// M1 task 6: "real tests for the allocators, including alignment and
// exhaustion behaviour").
#include "ps2ur/alloc.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <set>

using namespace ps2ur;

static bool is_aligned(const void* p, size_t align)
{
    return (reinterpret_cast<uintptr_t>(p) & (align - 1)) == 0;
}

// ---- StackAllocator --------------------------------------------------------

TEST(StackAllocator, BasicAllocationAndStats)
{
    alignas(16) unsigned char buffer[256];
    StackAllocator stack;
    stack.init(buffer, sizeof(buffer));

    EXPECT_EQ(stack.stats().capacity, sizeof(buffer));
    EXPECT_EQ(stack.remaining(), sizeof(buffer));

    void* a = stack.alloc(32);
    ASSERT_NE(a, nullptr);
    EXPECT_TRUE(is_aligned(a, 16));
    EXPECT_EQ(stack.stats().alloc_count, 1u);
    std::memset(a, 0x11, 32);
}

TEST(StackAllocator, DefaultAlignmentIsQword)
{
    // 16 bytes is the natural EE unit (128-bit regs, DMA) -- plan section 3.1.
    alignas(16) unsigned char buffer[256];
    StackAllocator stack;
    stack.init(buffer, sizeof(buffer));

    (void)stack.alloc(1, 1);
    void* p = stack.alloc(8);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(is_aligned(p, 16));
}

TEST(StackAllocator, RespectsExplicitAlignment)
{
    alignas(128) unsigned char buffer[1024];
    StackAllocator stack;
    stack.init(buffer, sizeof(buffer));

    (void)stack.alloc(3, 1);
    void* p32 = stack.alloc(8, 32);
    void* p128 = stack.alloc(8, 128);
    ASSERT_NE(p32, nullptr);
    ASSERT_NE(p128, nullptr);
    EXPECT_TRUE(is_aligned(p32, 32));
    EXPECT_TRUE(is_aligned(p128, 128));
}

TEST(StackAllocator, RewindToMarkerFreesEverythingAfter)
{
    alignas(16) unsigned char buffer[256];
    StackAllocator stack;
    stack.init(buffer, sizeof(buffer));

    void* first = stack.alloc(16);
    ASSERT_NE(first, nullptr);

    const StackAllocator::Marker mark = stack.marker();
    void* second = stack.alloc(64);
    ASSERT_NE(second, nullptr);
    EXPECT_LT(stack.remaining(), sizeof(buffer) - 16);

    stack.rewind(mark);

    // The next allocation reuses exactly the rewound space.
    void* reused = stack.alloc(64);
    EXPECT_EQ(reused, second);
    // and the allocation before the marker is untouched.
    EXPECT_NE(reused, first);
}

TEST(StackAllocator, NestedMarkersUnwindInOrder)
{
    alignas(16) unsigned char buffer[512];
    StackAllocator stack;
    stack.init(buffer, sizeof(buffer));

    const StackAllocator::Marker outer = stack.marker();
    void* a = stack.alloc(32);
    const StackAllocator::Marker inner = stack.marker();
    void* b = stack.alloc(32);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    stack.rewind(inner);
    EXPECT_EQ(stack.alloc(32), b);

    // Rewinding to an older marker frees more, including 'a'.
    stack.rewind(outer);
    EXPECT_EQ(stack.alloc(32), a);
}

TEST(StackAllocator, ExhaustionReturnsNullAndCounts)
{
    alignas(16) unsigned char buffer[64];
    StackAllocator stack;
    stack.init(buffer, sizeof(buffer));

    ASSERT_NE(stack.alloc(48, 16), nullptr);
    EXPECT_EQ(stack.alloc(64, 16), nullptr);
    EXPECT_EQ(stack.stats().fail_count, 1u);

    // Still usable for something that fits.
    EXPECT_NE(stack.alloc(8, 16), nullptr);
}

TEST(StackAllocator, ResetKeepsPeak)
{
    alignas(16) unsigned char buffer[128];
    StackAllocator stack;
    stack.init(buffer, sizeof(buffer));

    ASSERT_NE(stack.alloc(80, 16), nullptr);
    const size_t peak = stack.stats().peak;
    EXPECT_EQ(peak, 80u);

    stack.reset();
    EXPECT_EQ(stack.stats().used, 0u);
    EXPECT_EQ(stack.stats().peak, peak);
    EXPECT_EQ(stack.remaining(), sizeof(buffer));
}

// ---- PoolAllocator ---------------------------------------------------------

TEST(PoolAllocator, CarvesExpectedBlockCount)
{
    alignas(16) unsigned char buffer[256];
    PoolAllocator pool;
    pool.init(buffer, sizeof(buffer), 32, 16);

    EXPECT_EQ(pool.block_size(), 32u);
    EXPECT_EQ(pool.capacity(), 8u); // 256 / 32
    EXPECT_EQ(pool.used(), 0u);
}

TEST(PoolAllocator, RoundsBlockSizeUpToAlignment)
{
    alignas(16) unsigned char buffer[256];
    PoolAllocator pool;
    pool.init(buffer, sizeof(buffer), 20, 16); // 20 -> 32

    EXPECT_EQ(pool.block_size(), 32u);
    EXPECT_EQ(pool.capacity(), 8u);
}

TEST(PoolAllocator, AllBlocksAreDistinctAndAligned)
{
    alignas(16) unsigned char buffer[256];
    PoolAllocator pool;
    pool.init(buffer, sizeof(buffer), 32, 16);

    std::set<void*> seen;
    for (size_t i = 0; i < pool.capacity(); ++i) {
        void* p = pool.alloc();
        ASSERT_NE(p, nullptr) << "block " << i;
        EXPECT_TRUE(is_aligned(p, 16));
        EXPECT_TRUE(seen.insert(p).second) << "duplicate block at " << i;
        std::memset(p, 0x22, 32); // whole block is writable
    }
    EXPECT_EQ(pool.used(), pool.capacity());
}

TEST(PoolAllocator, ExhaustionReturnsNull)
{
    alignas(16) unsigned char buffer[128];
    PoolAllocator pool;
    pool.init(buffer, sizeof(buffer), 32, 16);

    for (size_t i = 0; i < pool.capacity(); ++i) {
        ASSERT_NE(pool.alloc(), nullptr);
    }
    EXPECT_EQ(pool.alloc(), nullptr);
    EXPECT_EQ(pool.stats().fail_count, 1u);
}

TEST(PoolAllocator, FreeMakesBlockAvailableAgain)
{
    alignas(16) unsigned char buffer[128];
    PoolAllocator pool;
    pool.init(buffer, sizeof(buffer), 32, 16);

    void* first = pool.alloc();
    ASSERT_NE(first, nullptr);
    while (pool.alloc() != nullptr) {
    }
    EXPECT_EQ(pool.used(), pool.capacity());

    pool.free(first);
    EXPECT_EQ(pool.used(), pool.capacity() - 1);

    // Most-recently-freed comes back first (LIFO free list).
    void* again = pool.alloc();
    EXPECT_EQ(again, first);
}

TEST(PoolAllocator, FreeNullIsNoOp)
{
    alignas(16) unsigned char buffer[128];
    PoolAllocator pool;
    pool.init(buffer, sizeof(buffer), 32, 16);

    ASSERT_NE(pool.alloc(), nullptr);
    const size_t used_before = pool.used();
    pool.free(nullptr);
    EXPECT_EQ(pool.used(), used_before);
}

TEST(PoolAllocator, AllocFreeChurnDoesNotCorruptFreeList)
{
    alignas(16) unsigned char buffer[512];
    PoolAllocator pool;
    pool.init(buffer, sizeof(buffer), 32, 16);

    // Churn well past capacity; a corrupted free list shows up as a duplicate
    // or an out-of-range pointer.
    void* held[4] = {nullptr, nullptr, nullptr, nullptr};
    for (int round = 0; round < 200; ++round) {
        const int slot = round % 4;
        if (held[slot] != nullptr) {
            pool.free(held[slot]);
        }
        held[slot] = pool.alloc();
        ASSERT_NE(held[slot], nullptr) << "round " << round;
        EXPECT_GE(held[slot], static_cast<void*>(buffer));
        EXPECT_LT(held[slot], static_cast<void*>(buffer + sizeof(buffer)));
        std::memset(held[slot], round & 0xFF, 32);
    }
    for (int i = 0; i < 4; ++i) {
        pool.free(held[i]);
    }
    EXPECT_EQ(pool.used(), 0u);
}

TEST(PoolAllocator, HandlesUnalignedBackingMemory)
{
    // Deliberately offset the backing store: init must align up and lose the
    // partial block rather than hand out a misaligned pointer.
    alignas(16) unsigned char buffer[256];
    PoolAllocator pool;
    pool.init(buffer + 1, sizeof(buffer) - 1, 32, 16);

    void* p = pool.alloc();
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(is_aligned(p, 16));
    EXPECT_LE(pool.capacity(), 7u);
}

// ---- Scratchpad ------------------------------------------------------------

TEST(Scratchpad, SizeMatchesHardware)
{
    // 16 KB of single-cycle SPR at 0x70000000 -- plan section 3.1.
    EXPECT_EQ(scratchpad::size(), 16u * 1024u);
}

TEST(Scratchpad, InitProvidesUsableArena)
{
    scratchpad::init();
    ASSERT_NE(scratchpad::base(), nullptr);

    Arena& arena = scratchpad::arena();
    EXPECT_EQ(arena.stats().capacity, scratchpad::size());

    void* p = arena.alloc(1024, 16);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(is_aligned(p, 16));
    std::memset(p, 0x33, 1024);

    arena.reset();
    EXPECT_EQ(arena.remaining(), scratchpad::size());
    scratchpad::shutdown();
    EXPECT_EQ(scratchpad::base(), nullptr);
}

TEST(Scratchpad, InitIsIdempotent)
{
    scratchpad::init();
    void* first = scratchpad::base();
    scratchpad::init();
    EXPECT_EQ(scratchpad::base(), first);
    scratchpad::shutdown();
}

TEST(Scratchpad, ArenaExhaustsAtCapacity)
{
    scratchpad::init();
    Arena& arena = scratchpad::arena();

    void* all = arena.alloc(scratchpad::size(), 16);
    ASSERT_NE(all, nullptr);
    EXPECT_EQ(arena.remaining(), 0u);
    EXPECT_EQ(arena.alloc(1, 1), nullptr);

    scratchpad::shutdown();
}
