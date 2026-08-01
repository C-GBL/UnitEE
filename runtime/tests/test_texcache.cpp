// Texture cache residency tests (plan section 9, M3 task 2).
//
// These run on the host build: GsDevice's VRAM allocator and packet builder are
// pure logic, and its GS-specific calls compile out, so the whole LRU and
// eviction policy is testable without hardware. That is exactly the leverage
// plan section 14.1 argues for.
#include "ps2ur/gs_texcache.h"

#include <gtest/gtest.h>

#include <vector>

using namespace ps2ur;
using namespace ps2ur::gfx;

namespace {

// A device configured small enough that the tests can exhaust it quickly.
struct Fixture {
    GsDevice device;
    TextureCache cache;
    std::vector<std::vector<uint8_t>> storage;

    Fixture(uint32_t budget_pages)
    {
        VideoConfig cfg;
        cfg.width = 512;
        cfg.height = 448;
        // Uploads are appended to the frame packet, so it must be big enough
        // for the worst-case number of misses in one frame: a 128x64 PSMT8
        // texture is 512 qwords of image data, and these tests deliberately
        // thrash. Undersizing this shows up as bind() failing, not as a
        // dropped upload.
        cfg.packet_qwords = 32768;
        EXPECT_TRUE(device.init(cfg));
        cache.init(budget_pages);
    }

    ~Fixture() { device.shutdown(); }

    // 128x64 PSMT8 is exactly one page, which makes budget arithmetic obvious.
    TextureCache::Handle add_one_page(const char* name)
    {
        storage.emplace_back(128 * 64, 0x7F);
        return cache.add(storage.back().data(), 128, 64, PixelFormat::PSMT8,
                         nullptr, 0, name);
    }

    // A real frame resets the device packet as well as the cache counters;
    // forgetting the former makes uploads accumulate until the packet
    // overflows and binds start failing.
    void frame()
    {
        device.begin_frame();
        cache.begin_frame();
    }
};

} // namespace

TEST(TextureCache, AddRejectsTextureLargerThanBudget)
{
    Fixture f(2);
    std::vector<uint8_t> big(256 * 256 * 4);
    // 256x256 PSMCT32 is 32 pages; the budget is 2.
    EXPECT_EQ(f.cache.add(big.data(), 256, 256, PixelFormat::PSMCT32, nullptr, 0, "big"),
              TextureCache::kInvalidHandle);
}

TEST(TextureCache, FirstBindIsAMissAndUploads)
{
    Fixture f(8);
    const auto h = f.add_one_page("a");
    ASSERT_NE(h, TextureCache::kInvalidHandle);

    f.frame();
    EXPECT_TRUE(f.cache.bind(f.device, h));
    EXPECT_EQ(f.cache.stats().misses, 1u);
    EXPECT_EQ(f.cache.stats().hits, 0u);
    EXPECT_EQ(f.cache.stats().uploads_this_frame, 1u);
    EXPECT_EQ(f.cache.stats().resident, 1u);
}

TEST(TextureCache, SecondBindIsAHitAndDoesNotReupload)
{
    Fixture f(8);
    const auto h = f.add_one_page("a");
    f.frame();
    ASSERT_TRUE(f.cache.bind(f.device, h));
    ASSERT_TRUE(f.cache.bind(f.device, h));

    EXPECT_EQ(f.cache.stats().misses, 1u);
    EXPECT_EQ(f.cache.stats().hits, 1u);
    EXPECT_EQ(f.cache.stats().uploads_this_frame, 1u) << "must not re-upload";
}

TEST(TextureCache, EvictsLeastRecentlyUsedWhenFull)
{
    Fixture f(3);
    const auto a = f.add_one_page("a");
    const auto b = f.add_one_page("b");
    const auto c = f.add_one_page("c");
    const auto d = f.add_one_page("d");

    f.frame();
    ASSERT_TRUE(f.cache.bind(f.device, a));
    ASSERT_TRUE(f.cache.bind(f.device, b));
    ASSERT_TRUE(f.cache.bind(f.device, c));
    EXPECT_EQ(f.cache.stats().resident, 3u);

    // Touch 'a' so 'b' becomes the least recently used.
    ASSERT_TRUE(f.cache.bind(f.device, a));

    ASSERT_TRUE(f.cache.bind(f.device, d));
    EXPECT_EQ(f.cache.stats().evictions_this_frame, 1u);
    EXPECT_EQ(f.cache.stats().resident, 3u);

    // 'a' should still be resident (a hit), 'b' evicted (a miss).
    const uint32_t hits_before = f.cache.stats().hits;
    ASSERT_TRUE(f.cache.bind(f.device, a));
    EXPECT_EQ(f.cache.stats().hits, hits_before + 1u) << "'a' was touched, must survive";

    const uint32_t misses_before = f.cache.stats().misses;
    ASSERT_TRUE(f.cache.bind(f.device, b));
    EXPECT_EQ(f.cache.stats().misses, misses_before + 1u) << "'b' was LRU, must be gone";
}

TEST(TextureCache, PinnedTexturesAreNeverEvicted)
{
    Fixture f(2);
    const auto font = f.add_one_page("font");
    const auto a = f.add_one_page("a");
    const auto b = f.add_one_page("b");

    f.frame();
    ASSERT_TRUE(f.cache.bind(f.device, font));
    f.cache.set_pinned(font, true);
    ASSERT_TRUE(f.cache.bind(f.device, a));

    // Binding 'b' must evict 'a', not the pinned font, even though the font is
    // older.
    ASSERT_TRUE(f.cache.bind(f.device, b));

    const uint32_t hits_before = f.cache.stats().hits;
    ASSERT_TRUE(f.cache.bind(f.device, font));
    EXPECT_EQ(f.cache.stats().hits, hits_before + 1u) << "pinned font must be resident";
}

TEST(TextureCache, FailsCleanlyWhenEverythingResidentIsPinned)
{
    Fixture f(2);
    const auto a = f.add_one_page("a");
    const auto b = f.add_one_page("b");
    const auto c = f.add_one_page("c");

    f.frame();
    ASSERT_TRUE(f.cache.bind(f.device, a));
    ASSERT_TRUE(f.cache.bind(f.device, b));
    f.cache.set_pinned(a, true);
    f.cache.set_pinned(b, true);

    // No eviction candidate: this must fail rather than corrupt VRAM.
    EXPECT_FALSE(f.cache.bind(f.device, c));
    EXPECT_EQ(f.cache.stats().failed_binds, 1u);
}

TEST(TextureCache, SurvivesWorkingSetLargerThanCapacity)
{
    // Plan M3 acceptance: "Cache handles a working set 3x VRAM capacity
    // without visual corruption." Here: 24 textures through an 8-page budget,
    // cycled repeatedly.
    Fixture f(8);
    std::vector<TextureCache::Handle> handles;
    for (int i = 0; i < 24; ++i) {
        handles.push_back(f.add_one_page("t"));
        ASSERT_NE(handles.back(), TextureCache::kInvalidHandle);
    }

    for (int frame = 0; frame < 10; ++frame) {
        f.frame();
        for (auto h : handles) {
            ASSERT_TRUE(f.cache.bind(f.device, h)) << "frame " << frame;
        }
        // Residency must never exceed the budget, however hard it is thrashed.
        EXPECT_LE(f.cache.stats().resident_pages, 8u);
    }
    EXPECT_EQ(f.cache.stats().failed_binds, 0u);
    // A 24-texture working set in an 8-page budget must thrash; if it did not,
    // the test is not exercising eviction.
    EXPECT_GT(f.cache.stats().evictions_this_frame, 0u);
}

TEST(TextureCache, TracksPeakUploadsPerFrame)
{
    Fixture f(4);
    std::vector<TextureCache::Handle> handles;
    for (int i = 0; i < 8; ++i) {
        handles.push_back(f.add_one_page("t"));
    }

    f.frame();
    for (auto h : handles) {
        ASSERT_TRUE(f.cache.bind(f.device, h));
    }
    EXPECT_EQ(f.cache.stats().uploads_this_frame, 8u);

    f.frame(); // rolls the peak
    EXPECT_EQ(f.cache.stats().peak_uploads_per_frame, 8u);
    EXPECT_EQ(f.cache.stats().uploads_this_frame, 0u);
}

TEST(TextureCache, IndexedTextureAlsoReservesClutSpace)
{
    Fixture f(8);
    std::vector<uint8_t> pixels(128 * 64, 0x21);
    std::vector<uint32_t> clut(256, 0xFF00FF00u);
    const auto h = f.cache.add(pixels.data(), 128, 64, PixelFormat::PSMT8,
                               clut.data(), 256, "indexed");
    ASSERT_NE(h, TextureCache::kInvalidHandle);

    f.frame();
    ASSERT_TRUE(f.cache.bind(f.device, h));
    // One page of texels plus one page for the CLUT.
    EXPECT_EQ(f.cache.stats().resident_pages, 2u);
}

TEST(TextureCache, FormatStatsProducesSomethingPrintable)
{
    Fixture f(8);
    const auto h = f.add_one_page("a");
    f.frame();
    ASSERT_TRUE(f.cache.bind(f.device, h));

    char buffer[128];
    f.cache.format_stats(buffer, sizeof(buffer));
    EXPECT_GT(std::strlen(buffer), 10u);
    EXPECT_NE(nullptr, std::strstr(buffer, "TEX"));
}
