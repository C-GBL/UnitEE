// Memory instrumentation tests (plan section 9, M13 task 2).
//
// The reason these exist: D4 says peak RAM stays under 30 MB of the 32 with
// no allocation failure across a 30-minute soak. That claim is only as good
// as the instrument making it, so the instrument gets tested against
// allocators whose exact behaviour is known.
#include "ps2ur/meminfo.h"

#include "ps2ur/alloc.h"

#include <gtest/gtest.h>

using namespace ps2ur;

namespace {

class MemInfoTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        mem::reset();
        heap_set_budget(0);
        heap_reset_stats();
    }
    void TearDown() override
    {
        mem::reset();
        heap_set_budget(0);
    }

    alignas(16) unsigned char m_block[4096] = {};
};

} // namespace

TEST_F(MemInfoTest, ReportsAnArenasHighWaterMarkAfterItIsReset)
{
    Arena a;
    a.init(m_block, sizeof(m_block));
    ASSERT_TRUE(mem::add_arena("scene", &a));

    ASSERT_NE(nullptr, a.alloc(1024));
    ASSERT_NE(nullptr, a.alloc(512));
    a.reset(); // frees everything; the peak must survive

    mem::Region r;
    ASSERT_TRUE(mem::find("scene", &r));
    EXPECT_EQ(sizeof(m_block), r.capacity);
    EXPECT_EQ(0u, r.used);
    EXPECT_GE(r.peak, 1536u);
    // The whole point of a high-water mark is that it outlives the release.
    EXPECT_GT(r.peak, r.used);
}

TEST_F(MemInfoTest, ReportsPoolBlockOccupancy)
{
    PoolAllocator p;
    p.init(m_block, sizeof(m_block), 256);
    ASSERT_TRUE(mem::add_pool("particles", &p));

    void* a = p.alloc();
    void* b = p.alloc();
    ASSERT_NE(nullptr, a);
    ASSERT_NE(nullptr, b);
    p.free(a);

    mem::Region r;
    ASSERT_TRUE(mem::find("particles", &r));
    EXPECT_EQ(256u, r.block_size);
    EXPECT_EQ(1u, r.blocks_used);
    EXPECT_EQ(sizeof(m_block) / 256, r.blocks_total);
    EXPECT_EQ(256u, r.used);
}

TEST_F(MemInfoTest, CountsAllocationFailuresRatherThanHidingThem)
{
    Arena a;
    a.init(m_block, 64);
    ASSERT_TRUE(mem::add_arena("tiny", &a));

    EXPECT_EQ(nullptr, a.alloc(4096)); // cannot fit

    mem::Region r;
    ASSERT_TRUE(mem::find("tiny", &r));
    EXPECT_EQ(1u, r.failures);
    EXPECT_EQ(1u, mem::totals().failures);
}

TEST_F(MemInfoTest, SumsTotalsAcrossEveryRegisteredAllocator)
{
    Arena a;
    StackAllocator s;
    a.init(m_block, 2048);
    s.init(m_block + 2048, 2048);
    ASSERT_TRUE(mem::add_arena("a", &a));
    ASSERT_TRUE(mem::add_stack("s", &s));

    ASSERT_NE(nullptr, a.alloc(256));
    ASSERT_NE(nullptr, s.alloc(128));

    const mem::Totals t = mem::totals();
    EXPECT_EQ(4096u, t.capacity);
    EXPECT_GE(t.used, 384u);
    EXPECT_EQ(2u, mem::count());
}

TEST_F(MemInfoTest, TracksHeapOutstandingAndPeak)
{
    ASSERT_TRUE(mem::add_heap("heap"));

    void* a = heap_alloc(4096);
    void* b = heap_alloc(2048);
    ASSERT_NE(nullptr, a);
    ASSERT_NE(nullptr, b);
    EXPECT_EQ(6144u, heap_stats().outstanding);

    heap_free(b);
    EXPECT_EQ(4096u, heap_stats().outstanding);
    EXPECT_EQ(6144u, heap_stats().peak); // the mark survives the free

    heap_free(a);
    EXPECT_EQ(0u, heap_stats().outstanding);
    EXPECT_EQ(2u, heap_stats().frees);
}

TEST_F(MemInfoTest, HeapBudgetRefusesRatherThanOverspending)
{
    // On a machine with no virtual memory, the failure has to happen at the
    // call site that asked for too much, not later somewhere unrelated.
    heap_set_budget(8192);

    void* a = heap_alloc(4096);
    ASSERT_NE(nullptr, a);
    void* b = heap_alloc(8192); // would cross the ceiling
    EXPECT_EQ(nullptr, b);
    EXPECT_EQ(1u, heap_stats().failures);
    EXPECT_EQ(4096u, heap_stats().outstanding);

    // Freeing makes room again.
    heap_free(a);
    void* c = heap_alloc(8192);
    EXPECT_NE(nullptr, c);
    heap_free(c);
}

TEST_F(MemInfoTest, ChecksBudgetsAgainstPeakNotCurrentUse)
{
    Arena a;
    a.init(m_block, sizeof(m_block));
    ASSERT_TRUE(mem::add_arena("assets", &a));

    ASSERT_NE(nullptr, a.alloc(3072));
    a.reset(); // current use is now zero

    // A budget met on average and blown during a load screen is blown. The
    // check must see the peak, or it would pass here and fail on a console.
    const mem::Budget over[] = {{"assets", 1024}};
    EXPECT_EQ(1u, mem::check(over, 1));

    const mem::Budget under[] = {{"assets", 8192}};
    EXPECT_EQ(0u, mem::check(under, 1));
}

TEST_F(MemInfoTest, ReportsAMissingBudgetRegionWithoutCountingItAsOverBudget)
{
    const mem::Budget b[] = {{"not-registered", 1024}};
    EXPECT_EQ(0u, mem::check(b, 1));
}

TEST_F(MemInfoTest, ExternalRegionsReportThroughTheirCallback)
{
    static size_t s_used = 0;
    s_used = 512 * 1024;
    ASSERT_TRUE(mem::add_external(
        "vram", 4u * 1024u * 1024u,
        [](void* ctx) -> size_t { return *static_cast<size_t*>(ctx); }, &s_used));

    mem::Region r;
    ASSERT_TRUE(mem::find("vram", &r));
    EXPECT_EQ(4u * 1024u * 1024u, r.capacity);
    EXPECT_EQ(512u * 1024u, r.used);

    s_used = 1024 * 1024;
    ASSERT_TRUE(mem::find("vram", &r));
    EXPECT_EQ(1024u * 1024u, r.used); // polled, not cached
}

TEST_F(MemInfoTest, HeapFragmentationIsZeroWhenNothingHasBeenFreed)
{
    ASSERT_TRUE(mem::add_heap("heap"));
    void* a = heap_alloc(4096);
    ASSERT_NE(nullptr, a);
    EXPECT_FLOAT_EQ(0.0f, mem::heap_fragmentation());

    heap_free(a);
    // Everything returned: the whole peak footprint is now idle.
    EXPECT_GT(mem::heap_fragmentation(), 0.9f);
}

TEST_F(MemInfoTest, ToleratesMoreRegionsThanTheRegistryHolds)
{
    static Arena arenas[mem::kMaxRegions + 4];
    char names[mem::kMaxRegions + 4][8];
    for (uint32_t i = 0; i < mem::kMaxRegions + 4; ++i) {
        std::snprintf(names[i], sizeof(names[i]), "r%u", static_cast<unsigned>(i));
        arenas[i].init(nullptr, 0);
        mem::add_arena(names[i], &arenas[i]);
    }
    EXPECT_EQ(mem::kMaxRegions, mem::count());
    // The registry being full must not take the frame down with it.
    EXPECT_NO_FATAL_FAILURE(mem::log_map());
}
