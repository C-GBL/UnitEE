// Arena allocator + heap facade unit tests.
#include "ps2ur/alloc.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

using namespace ps2ur;

static bool is_aligned(const void* p, size_t align)
{
    return (reinterpret_cast<uintptr_t>(p) & (align - 1)) == 0;
}

TEST(Arena, BasicAllocationAndStats)
{
    alignas(16) unsigned char buffer[256];
    Arena arena;
    arena.init(buffer, sizeof(buffer));

    EXPECT_EQ(arena.stats().capacity, sizeof(buffer));
    EXPECT_EQ(arena.stats().used, 0u);

    void* a = arena.alloc(10);
    ASSERT_NE(a, nullptr);
    EXPECT_TRUE(is_aligned(a, 16));
    EXPECT_EQ(arena.stats().alloc_count, 1u);
    EXPECT_EQ(arena.stats().used, 10u);

    // Memory is usable.
    std::memset(a, 0xAB, 10);
}

TEST(Arena, RespectsAlignment)
{
    alignas(64) unsigned char buffer[512];
    Arena arena;
    arena.init(buffer, sizeof(buffer));

    (void)arena.alloc(1, 1); // knock the offset off alignment
    void* p16 = arena.alloc(8, 16);
    void* p64 = arena.alloc(8, 64);
    ASSERT_NE(p16, nullptr);
    ASSERT_NE(p64, nullptr);
    EXPECT_TRUE(is_aligned(p16, 16));
    EXPECT_TRUE(is_aligned(p64, 64));
}

TEST(Arena, ExhaustionReturnsNull)
{
    alignas(16) unsigned char buffer[64];
    Arena arena;
    arena.init(buffer, sizeof(buffer));

    void* ok = arena.alloc(48, 16);
    ASSERT_NE(ok, nullptr);

    void* too_big = arena.alloc(64, 16);
    EXPECT_EQ(too_big, nullptr);
    EXPECT_EQ(arena.stats().fail_count, 1u);

    // A fitting allocation still succeeds after a failure.
    void* fits = arena.alloc(8, 16);
    EXPECT_NE(fits, nullptr);
}

TEST(Arena, ResetReclaimsSpaceKeepsPeak)
{
    alignas(16) unsigned char buffer[128];
    Arena arena;
    arena.init(buffer, sizeof(buffer));

    void* a = arena.alloc(96, 16);
    ASSERT_NE(a, nullptr);
    const size_t peak_before = arena.stats().peak;
    EXPECT_EQ(peak_before, 96u);

    arena.reset();
    EXPECT_EQ(arena.stats().used, 0u);
    EXPECT_EQ(arena.stats().peak, peak_before); // peak survives reset
    EXPECT_EQ(arena.remaining(), sizeof(buffer));

    // Full capacity available again; same base address comes back.
    void* b = arena.alloc(96, 16);
    EXPECT_EQ(b, a);
}

TEST(Arena, ZeroRemainingAfterExactFill)
{
    alignas(16) unsigned char buffer[64];
    Arena arena;
    arena.init(buffer, sizeof(buffer));

    void* all = arena.alloc(64, 16);
    ASSERT_NE(all, nullptr);
    EXPECT_EQ(arena.remaining(), 0u);
    EXPECT_EQ(arena.alloc(1, 1), nullptr);
}

TEST(Heap, AlignedAllocFree)
{
    void* p = heap_alloc(100, 64);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(is_aligned(p, 64));
    std::memset(p, 0xCD, 100);
    heap_free(p);

    void* q = heap_alloc(1, 256);
    ASSERT_NE(q, nullptr);
    EXPECT_TRUE(is_aligned(q, 256));
    heap_free(q);

    heap_free(nullptr); // null-safe
}
