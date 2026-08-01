// Swizzle, CLUT ordering and PS2 alpha range tests (plan section 9, M3 task 1).
//
// Scope note: texture swizzling is NOT tested here because it is not done in
// software -- the GS transfer engine swizzles during a host->local upload
// (verified by samples/07-swizzle). What remains is the CLUT reordering, which
// the hardware does NOT do for us, and the PS2 alpha range.
#include "ps2ur/gs_swizzle.h"

#include <gtest/gtest.h>

#include <cstring>
#include <set>

using namespace ps2ur;
using namespace ps2ur::gfx;

// ---- CLUT CSM1 ordering ----------------------------------------------------

TEST(ClutCsm1, SwapsBlocksOneTwoAndFiveSix)
{
    uint32_t src[256];
    uint32_t dst[256];
    for (uint32_t i = 0; i < 256; ++i) {
        src[i] = i;
    }
    clut_csm1_reorder(src, dst, 256);

    // Within the first group of 8 blocks: block 1 (entries 8..15) must land
    // where block 2 (entries 16..23) was, and vice versa.
    for (uint32_t i = 0; i < 8; ++i) {
        EXPECT_EQ(dst[16 + i], 8 + i) << "block 1 -> 2, entry " << i;
        EXPECT_EQ(dst[8 + i], 16 + i) << "block 2 -> 1, entry " << i;
        EXPECT_EQ(dst[48 + i], 40 + i) << "block 5 -> 6, entry " << i;
        EXPECT_EQ(dst[40 + i], 48 + i) << "block 6 -> 5, entry " << i;
    }
    // Blocks 0, 3, 4, 7 stay put.
    for (uint32_t i = 0; i < 8; ++i) {
        EXPECT_EQ(dst[0 + i], 0 + i);
        EXPECT_EQ(dst[24 + i], 24 + i);
        EXPECT_EQ(dst[32 + i], 32 + i);
        EXPECT_EQ(dst[56 + i], 56 + i);
    }
}

TEST(ClutCsm1, AppliesToEveryGroupOfThirtyTwo)
{
    uint32_t src[256];
    uint32_t dst[256];
    for (uint32_t i = 0; i < 256; ++i) {
        src[i] = i;
    }
    clut_csm1_reorder(src, dst, 256);

    // The shuffle repeats in all 8 groups of 32 entries.
    for (uint32_t group = 0; group < 8; ++group) {
        const uint32_t base = group * 32u;
        for (uint32_t i = 0; i < 8; ++i) {
            EXPECT_EQ(dst[base + 16 + i], base + 8 + i) << "group " << group;
            EXPECT_EQ(dst[base + 8 + i], base + 16 + i) << "group " << group;
        }
    }
}

TEST(ClutCsm1, IsItsOwnInverse)
{
    uint32_t src[256], once[256], twice[256];
    for (uint32_t i = 0; i < 256; ++i) {
        src[i] = 0xDEAD0000u | i;
    }
    clut_csm1_reorder(src, once, 256);
    clut_csm1_reorder(once, twice, 256);
    EXPECT_EQ(0, std::memcmp(src, twice, sizeof(src)));
}

TEST(ClutCsm1, IsAPermutationNotALoss)
{
    uint32_t src[256], dst[256];
    for (uint32_t i = 0; i < 256; ++i) {
        src[i] = i;
    }
    clut_csm1_reorder(src, dst, 256);
    std::set<uint32_t> seen(dst, dst + 256);
    EXPECT_EQ(seen.size(), 256u) << "every entry must survive exactly once";
}

TEST(ClutCsm1, SixteenEntryPaletteIsLinear)
{
    // PSMT4's 16-entry CLUT has no block shuffle.
    uint32_t src[16], dst[16];
    for (uint32_t i = 0; i < 16; ++i) {
        src[i] = i * 7u;
    }
    clut_csm1_reorder(src, dst, 16);
    EXPECT_EQ(0, std::memcmp(src, dst, sizeof(src)));
}

// ---- PS2 alpha range -------------------------------------------------------

TEST(Ps2Alpha, OpaqueMapsTo128)
{
    // The one everybody gets wrong: 0xFF in, 0x80 out.
    EXPECT_EQ(alpha_to_ps2(255), 128);
    EXPECT_EQ(alpha_to_ps2(0), 0);
}

TEST(Ps2Alpha, HalfwayIsHalfway)
{
    EXPECT_EQ(alpha_to_ps2(128), 64);
    EXPECT_NEAR(alpha_to_ps2(64), 32, 1);
}

TEST(Ps2Alpha, RoundTripsWithinRoundingError)
{
    for (uint32_t a = 0; a <= 255; ++a) {
        const uint8_t there = alpha_to_ps2(static_cast<uint8_t>(a));
        const uint8_t back = alpha_from_ps2(there);
        EXPECT_LE(back > a ? back - a : a - back, 2u) << "alpha " << a;
    }
}

TEST(Ps2Alpha, NeverExceedsHardwareMaximum)
{
    for (uint32_t a = 0; a <= 255; ++a) {
        EXPECT_LE(alpha_to_ps2(static_cast<uint8_t>(a)), 128);
    }
}
