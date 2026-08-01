// VRAM allocator, GS format geometry, and GIF packet construction tests
// (plan section 9, M2 tasks 2-3).
//
// These matter more than usual: a wrong bit position in a GIF tag does not
// crash, it produces plausible-looking garbage on a screen you cannot attach a
// debugger to. Bit layouts are asserted against values decoded by hand from
// the register descriptions in plan section 3.3 and ps2sdk's headers.
#include "ps2ur/gs_format.h"
#include "ps2ur/gs_packet.h"
#include "ps2ur/gs_vram.h"

#include <gtest/gtest.h>

using namespace ps2ur;
using namespace ps2ur::gfx;

// ---- Format geometry -------------------------------------------------------

TEST(GsFormat, PageGeometryCoversExactlyEightKilobytes)
{
    const PixelFormat formats[] = {
        PixelFormat::PSMCT32, PixelFormat::PSMCT24, PixelFormat::PSMCT16,
        PixelFormat::PSMCT16S, PixelFormat::PSMT8,  PixelFormat::PSMT4,
        PixelFormat::PSMZ32,   PixelFormat::PSMZ24, PixelFormat::PSMZ16,
        PixelFormat::PSMZ16S,
    };
    for (PixelFormat fmt : formats) {
        const PageGeometry pg = page_geometry(fmt);
        const uint32_t bytes = pg.width * pg.height * bits_per_pixel(fmt) / 8u;
        EXPECT_EQ(bytes, kPageBytes)
            << "format 0x" << std::hex << static_cast<int>(fmt);
    }
}

TEST(GsFormat, BaselineFramebufferMatchesPlanBudget)
{
    // Plan section 15.2: two 512x448 PSMCT32 colour buffers = 1.75 MB,
    // PSMZ24 Z buffer = 0.88 MB.
    const uint32_t colour_pages = pages_for_buffer(512, 448, PixelFormat::PSMCT32);
    EXPECT_EQ(colour_pages, 112u);
    EXPECT_EQ(colour_pages * kPageBytes, 896u * 1024u);
    EXPECT_EQ(2u * colour_pages * kPageBytes, 1792u * 1024u); // 1.75 MB

    const uint32_t z_pages = pages_for_buffer(512, 448, PixelFormat::PSMZ24);
    EXPECT_EQ(z_pages * kPageBytes, 896u * 1024u);

    // The whole baseline config must fit with room for textures.
    const uint32_t total = 2u * colour_pages + z_pages;
    EXPECT_LT(total, kPageCount);
    EXPECT_GT((kPageCount - total) * kPageBytes, 1200u * 1024u); // ~1.2 MB left
}

TEST(GsFormat, SixteenBitConfigLeavesMoreTextureSpace)
{
    // Plan section 3.3's aggressive alternative: PSMCT16 + PSMZ16S ~= 1.4 MB.
    const uint32_t colour = pages_for_buffer(512, 448, PixelFormat::PSMCT16);
    const uint32_t z = pages_for_buffer(512, 448, PixelFormat::PSMZ16S);
    const uint32_t total_bytes = (2u * colour + z) * kPageBytes;
    EXPECT_LE(total_bytes, 1500u * 1024u);
    EXPECT_GE((kVramBytes - total_bytes), 2500u * 1024u); // ~2.6 MB for textures
}

TEST(GsFormat, IndexedFormatsArePagedDensely)
{
    // A 256x256 PSMT8 texture is 64 KB (plan section 3.3).
    EXPECT_EQ(pages_for_buffer(256, 256, PixelFormat::PSMT8) * kPageBytes, 64u * 1024u);
    // The same texture as PSMCT32 is 256 KB -- 4x the cost.
    EXPECT_EQ(pages_for_buffer(256, 256, PixelFormat::PSMCT32) * kPageBytes, 256u * 1024u);
    // PSMT4 halves PSMT8 again.
    EXPECT_EQ(pages_for_buffer(256, 256, PixelFormat::PSMT4) * kPageBytes, 32u * 1024u);
}

TEST(GsFormat, BufferWidthUnitsAreSixtyFourPixels)
{
    EXPECT_EQ(buffer_width_units(512), 8u);
    EXPECT_EQ(buffer_width_units(640), 10u);
    EXPECT_EQ(buffer_width_units(64), 1u);
    EXPECT_EQ(buffer_width_units(65), 2u); // rounds up
}

// ---- VramAllocator ---------------------------------------------------------

TEST(VramAllocator, StartsEmpty)
{
    VramAllocator vram;
    vram.init();
    EXPECT_EQ(vram.used_pages(), 0u);
    EXPECT_EQ(vram.free_pages(), kPageCount);
    EXPECT_EQ(vram.largest_free_run(), kPageCount);
}

TEST(VramAllocator, AllocatesBaselineFrameAndZBuffers)
{
    VramAllocator vram;
    vram.init();

    const VramAlloc fb0 = vram.alloc_buffer(512, 448, PixelFormat::PSMCT32, "frame0");
    const VramAlloc fb1 = vram.alloc_buffer(512, 448, PixelFormat::PSMCT32, "frame1");
    const VramAlloc zb = vram.alloc_buffer(512, 448, PixelFormat::PSMZ24, "z");

    ASSERT_TRUE(fb0.valid());
    ASSERT_TRUE(fb1.valid());
    ASSERT_TRUE(zb.valid());

    // Non-overlapping, in allocation order.
    EXPECT_EQ(fb0.page, 0u);
    EXPECT_EQ(fb1.page, 112u);
    EXPECT_EQ(zb.page, 224u);
    EXPECT_EQ(vram.used_pages(), 336u);

    // Textures still have room.
    EXPECT_GE(vram.free_pages() * kPageBytes, 1200u * 1024u);
}

TEST(VramAllocator, BlockAddressIsThirtyTwoTimesPage)
{
    VramAllocator vram;
    vram.init();
    const VramAlloc a = vram.alloc_pages(1, "a");
    const VramAlloc b = vram.alloc_pages(1, "b");
    ASSERT_TRUE(a.valid());
    ASSERT_TRUE(b.valid());

    // TEX0.TBP0 is in 256-byte blocks; one page is 32 blocks.
    EXPECT_EQ(a.block(), a.page * 32u);
    EXPECT_EQ(b.block(), 32u);
    EXPECT_EQ(b.byte_offset(), kPageBytes);
}

TEST(VramAllocator, FreeReturnsSpaceAndAllowsReuse)
{
    VramAllocator vram;
    vram.init();

    const VramAlloc a = vram.alloc_pages(10, "a");
    const VramAlloc b = vram.alloc_pages(10, "b");
    ASSERT_TRUE(a.valid());
    ASSERT_TRUE(b.valid());
    EXPECT_EQ(vram.used_pages(), 20u);

    vram.free(a);
    EXPECT_EQ(vram.used_pages(), 10u);

    // First fit reuses the hole 'a' left behind.
    const VramAlloc c = vram.alloc_pages(10, "c");
    ASSERT_TRUE(c.valid());
    EXPECT_EQ(c.page, a.page);
}

TEST(VramAllocator, FirstFitSkipsTooSmallHoles)
{
    VramAllocator vram;
    vram.init();

    const VramAlloc a = vram.alloc_pages(4, "a");
    const VramAlloc b = vram.alloc_pages(4, "b");
    const VramAlloc c = vram.alloc_pages(4, "c");
    ASSERT_TRUE(a.valid() && b.valid() && c.valid());

    vram.free(b); // 4-page hole at page 4

    const VramAlloc big = vram.alloc_pages(8, "big");
    ASSERT_TRUE(big.valid());
    EXPECT_EQ(big.page, 12u) << "must skip the 4-page hole and land after 'c'";

    const VramAlloc small = vram.alloc_pages(4, "small");
    ASSERT_TRUE(small.valid());
    EXPECT_EQ(small.page, 4u) << "the hole is exactly the right size";
}

TEST(VramAllocator, ExhaustionFailsCleanly)
{
    VramAllocator vram;
    vram.init();

    const VramAlloc all = vram.alloc_pages(kPageCount, "everything");
    ASSERT_TRUE(all.valid());
    EXPECT_EQ(vram.free_pages(), 0u);

    // Running out of VRAM is a normal budgeting outcome, not a crash.
    const VramAlloc none = vram.alloc_pages(1, "one-too-many");
    EXPECT_FALSE(none.valid());

    vram.free(all);
    EXPECT_EQ(vram.free_pages(), kPageCount);
}

TEST(VramAllocator, OversizedRequestFails)
{
    VramAllocator vram;
    vram.init();
    EXPECT_FALSE(vram.alloc_pages(kPageCount + 1u, "too big").valid());
    EXPECT_FALSE(vram.alloc_pages(0, "zero").valid());
    // A 1024x1024 PSMCT32 texture is 4 MB -- the entire VRAM, so it cannot
    // coexist with a framebuffer. Worth proving the allocator says so.
    ASSERT_TRUE(vram.alloc_buffer(512, 448, PixelFormat::PSMCT32, "frame").valid());
    EXPECT_FALSE(vram.alloc_buffer(1024, 1024, PixelFormat::PSMCT32, "huge").valid());
}

TEST(VramAllocator, ResetClearsEverything)
{
    VramAllocator vram;
    vram.init();
    ASSERT_TRUE(vram.alloc_pages(100, "a").valid());
    vram.reset();
    EXPECT_EQ(vram.used_pages(), 0u);
    EXPECT_EQ(vram.largest_free_run(), kPageCount);
}

// ---- GIF tag / register encoding -------------------------------------------

TEST(GsPacket, PackedAdTagLayout)
{
    alignas(16) Qword buffer[8];
    GsPacket packet;
    packet.init(buffer, 8);

    packet.begin_packed_ad(2);
    ASSERT_EQ(packet.size(), 1u);

    const uint64_t lo = buffer[0].lo;
    EXPECT_EQ(lo & 0x7FFFu, 2u) << "NLOOP";
    EXPECT_EQ((lo >> 15) & 1u, 0u) << "EOP clear";
    EXPECT_EQ((lo >> 58) & 3u, static_cast<uint64_t>(GifMode::Packed)) << "FLG";
    EXPECT_EQ((lo >> 60) & 0xFu, 1u) << "NREG";
    EXPECT_EQ(buffer[0].hi & 0xFu, kGifRegAD) << "REGS holds the A+D descriptor";
}

TEST(GsPacket, EndOfPacketBitAndRetroactiveSet)
{
    alignas(16) Qword buffer[8];
    GsPacket packet;
    packet.init(buffer, 8);

    packet.begin_packed_ad(1, /*end_of_packet=*/true);
    EXPECT_EQ((buffer[0].lo >> 15) & 1u, 1u);

    packet.reset();
    packet.begin_packed_ad(1);
    EXPECT_EQ((buffer[0].lo >> 15) & 1u, 0u);
    packet.set_last_tag_eop();
    EXPECT_EQ((buffer[0].lo >> 15) & 1u, 1u);
}

TEST(GsPacket, AdWritePutsRegisterInHighHalf)
{
    alignas(16) Qword buffer[8];
    GsPacket packet;
    packet.init(buffer, 8);

    packet.begin_packed_ad(1);
    packet.add_ad(GsReg::FRAME_1, 0x1234'5678'9ABC'DEF0ull);

    ASSERT_EQ(packet.size(), 2u);
    EXPECT_EQ(buffer[1].lo, 0x1234'5678'9ABC'DEF0ull);
    EXPECT_EQ(buffer[1].hi, static_cast<uint64_t>(GsReg::FRAME_1));
}

TEST(GsPacket, ImageModeTag)
{
    alignas(16) Qword buffer[8];
    GsPacket packet;
    packet.init(buffer, 8);

    packet.begin_image(4);
    EXPECT_EQ(buffer[0].lo & 0x7FFFu, 4u);
    EXPECT_EQ((buffer[0].lo >> 58) & 3u, static_cast<uint64_t>(GifMode::Image));
}

TEST(GsPacket, PrimFieldIsAppliedWhenRequested)
{
    alignas(16) Qword buffer[8];
    GsPacket packet;
    packet.init(buffer, 8);

    const uint64_t prim = gs_prim(GsPrim::Triangle, true, false, false, false,
                                  false, false, 0, false);
    packet.begin_packed(3, 2, gs_reglist(GsReg::RGBAQ, GsReg::XYZ2), false, true, prim);

    EXPECT_EQ((buffer[0].lo >> 46) & 1u, 1u) << "PRE set";
    EXPECT_EQ((buffer[0].lo >> 47) & 0x7FFu, prim) << "PRIM field";
    EXPECT_EQ((buffer[0].lo >> 60) & 0xFu, 2u) << "NREG";
    EXPECT_EQ(buffer[0].hi & 0xFu, static_cast<uint64_t>(GsReg::RGBAQ));
    EXPECT_EQ((buffer[0].hi >> 4) & 0xFu, static_cast<uint64_t>(GsReg::XYZ2));
}

TEST(GsPacket, OverflowIsStickyAndDoesNotWriteOutOfBounds)
{
    alignas(16) Qword buffer[4];
    // Sentinel past the end; a bounds bug would clobber it.
    alignas(16) Qword sentinel{0xDEADBEEFull, 0xCAFEBABEull};

    GsPacket packet;
    packet.init(buffer, 4);
    for (int i = 0; i < 16; ++i) {
        packet.add_qword(static_cast<uint64_t>(i), 0);
    }

    EXPECT_TRUE(packet.overflowed());
    EXPECT_EQ(packet.size(), 4u) << "never advances past capacity";
    EXPECT_EQ(sentinel.lo, 0xDEADBEEFull);
    EXPECT_EQ(sentinel.hi, 0xCAFEBABEull);

    packet.reset();
    EXPECT_FALSE(packet.overflowed()) << "reset clears the sticky flag";
}

TEST(GsPacket, CoordinateConversionAppliesOriginBias)
{
    // Screen (0,0) sits at 2048 in GS 12.4 fixed point.
    EXPECT_EQ(gs_coord(0), 2048u << 4);
    EXPECT_EQ(gs_coord(1), (2048u << 4) + 16u);
    EXPECT_EQ(gs_coord(512), (2048u + 512u) << 4);
    // Sub-pixel: half a pixel is 8 sixteenths.
    EXPECT_EQ(gs_coord_fixed(8), (2048u << 4) + 8u);
}

TEST(GsRegisters, FrameAndZbufEncoding)
{
    // 512-wide PSMCT32 frame at page 0.
    const uint64_t frame = gs_frame(0, buffer_width_units(512), PixelFormat::PSMCT32, 0);
    EXPECT_EQ(frame & 0x1FFu, 0u) << "FBP";
    EXPECT_EQ((frame >> 16) & 0x3Fu, 8u) << "FBW in 64-pixel units";
    EXPECT_EQ((frame >> 24) & 0x3Fu, static_cast<uint64_t>(PixelFormat::PSMCT32));

    const uint64_t zbuf = gs_zbuf(224, PixelFormat::PSMZ24, false);
    EXPECT_EQ(zbuf & 0x1FFu, 224u) << "ZBP";
    EXPECT_EQ((zbuf >> 24) & 0xFu, static_cast<uint64_t>(PixelFormat::PSMZ24) & 0xFu);
}

TEST(GsRegisters, ScissorAndXyoffsetEncoding)
{
    const uint64_t scissor = gs_scissor(0, 511, 0, 447);
    EXPECT_EQ(scissor & 0x7FFu, 0u);
    EXPECT_EQ((scissor >> 16) & 0x7FFu, 511u);
    EXPECT_EQ((scissor >> 32) & 0x7FFu, 0u);
    EXPECT_EQ((scissor >> 48) & 0x7FFu, 447u);

    const uint64_t off = gs_xyoffset(kGsOriginX << 4, kGsOriginY << 4);
    EXPECT_EQ(off & 0xFFFFu, static_cast<uint64_t>(kGsOriginX << 4));
    EXPECT_EQ((off >> 32) & 0xFFFFu, static_cast<uint64_t>(kGsOriginY << 4));
}

TEST(GsRegisters, RgbaqUsesPs2AlphaRange)
{
    // PS2 alpha is 0-128, where 0x80 is fully opaque (plan section 9, M3:
    // "This trips up everyone once").
    const uint64_t c = gs_rgbaq(255, 128, 64, 0x80, 0x3F800000u);
    EXPECT_EQ(c & 0xFFu, 255u);
    EXPECT_EQ((c >> 8) & 0xFFu, 128u);
    EXPECT_EQ((c >> 16) & 0xFFu, 64u);
    EXPECT_EQ((c >> 24) & 0xFFu, 0x80u);
    EXPECT_EQ((c >> 32) & 0xFFFFFFFFu, 0x3F800000u) << "Q = 1.0f";
}

TEST(GsRegisters, TestRegisterEnablesDepth)
{
    // z_enable with ZTST=2 (gequal) is the standard opaque depth test.
    const uint64_t test = gs_test(false, 0, 0, 0, false, 0, true, 2);
    EXPECT_EQ((test >> 16) & 1u, 1u) << "ZTE";
    EXPECT_EQ((test >> 17) & 3u, 2u) << "ZTST gequal";
}
