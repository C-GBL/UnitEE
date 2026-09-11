// GS VRAM allocator (plan section 9, M2 task 3).
//
// 4 MB, page-granular (8 KB), first-fit over a 512-page bitmap. Framebuffers
// and the Z buffer are reserved once at startup; textures come and go, which
// is why this is a real allocator with free() rather than a bump pointer --
// M3's texture cache sits directly on top of it.
//
// Page granularity is not a simplification: FRAME.FBP and ZBUF.ZBP are
// expressed in page units, so a framebuffer *must* start on a page boundary.
// Textures are addressed in 256-byte blocks (TEX0.TBP0), and a page boundary
// is always a block boundary, so allocating textures by page satisfies them
// too -- at the cost of at most 8 KB of tail waste per texture.
#pragma once

#include "ps2ur/gs_format.h"

#include <cstdint>

namespace ps2ur {
namespace gfx {

// A VRAM reservation. 'page' is the index the GS register wants for a frame or
// Z buffer; block()/byte_offset() convert for other uses.
struct VramAlloc {
    uint32_t page = kInvalidPage;
    uint32_t page_count = 0;
    // Offset WITHIN the page, in 256-byte blocks. Zero for every page-granular
    // reservation, non-zero only for the sub-page CLUT slots below.
    uint32_t block_in_page = 0;

    static constexpr uint32_t kInvalidPage = 0xFFFFFFFFu;

    bool valid() const { return page != kInvalidPage; }
    // A sub-page slot rather than a run of whole pages.
    bool is_slot() const { return valid() && page_count == 0; }

    // TEX0.TBP0 and TEX0.CBP are both in 256-byte blocks.
    uint32_t block() const { return page * kBlocksPerPage + block_in_page; }
    uint32_t byte_offset() const
    {
        return page * kPageBytes + block_in_page * kBlockBytes;
    }
};

class VramAllocator {
public:
    static constexpr uint32_t kMaxAllocs = 64;

    void init();
    void reset();

    // Reserves 'count' contiguous pages. Returns an invalid VramAlloc when
    // there is no run long enough -- callers must check, since running out of
    // VRAM is a normal budgeting outcome on this machine, not an exception.
    VramAlloc alloc_pages(uint32_t count, const char* debug_name);

    // Convenience: reserve enough pages for a width x height buffer.
    VramAlloc alloc_buffer(uint32_t width, uint32_t height, PixelFormat fmt,
                           const char* debug_name);

    // A 256-entry CLUT is 1 KB, and pages are 8 KB. Handing every CLUT its
    // own page throws away 7 KB each: with two dozen textures that is around
    // 168 KB of a texture pool that only has about 1.2 MB to begin with, and
    // it is exactly why scenes were reporting textures skipped for want of
    // VRAM while the allocator claimed 98 percent occupancy (M13 task 3).
    //
    // The GS addresses a CLUT by 256-byte block, not by page, so eight of
    // them fit in one page with no loss of addressability. Slots come from
    // pages this allocator reserves on demand and never releases, because a
    // CLUT page that empties is almost certainly about to be refilled by the
    // next scene.
    VramAlloc alloc_clut(const char* debug_name);

    void free(const VramAlloc& alloc);

    uint32_t used_pages() const { return m_used_pages; }
    // Pages currently reserved to hold CLUT slots, and how many of those
    // slots are handed out. Reported by the VRAM budget line.
    uint32_t clut_pages() const { return m_clut_page_count; }
    uint32_t clut_slots_used() const;
    uint32_t free_pages() const { return kPageCount - m_used_pages; }
    uint32_t largest_free_run() const;

    // Prints the reservation table and a page-occupancy map to the log.
    // Required by M2 task 3; this is the tool you reach for when a texture
    // upload lands on top of the Z buffer.
    void debug_dump() const;

    // One line saying where the 4 MB went, split the way a person has to
    // think about it: the framebuffers and Z buffer are fixed by the video
    // mode, and everything left is the texture pool. "Out of VRAM" is only
    // actionable once you know which of those two is eating it (M13 task 3).
    void log_budget() const;

    // Read-only view of allocation slot 'i' (0 <= i < kMaxAllocs) for the
    // on-screen VRAM page: false for an unused slot. The debug name is the
    // one the caller passed to alloc_*; the device names its own buffers
    // "colour*" / "depth", content is "game-tex" / "game-clut" / fonts.
    bool record(uint32_t i, uint32_t& page, uint32_t& page_count,
                const char*& name) const;
    // True when page 'p' is marked used in the occupancy bitmap.
    bool page_used(uint32_t p) const;

private:
    struct Record {
        uint32_t page = 0;
        uint32_t page_count = 0;
        const char* name = nullptr;
        bool in_use = false;
    };

    bool is_free(uint32_t page, uint32_t count) const;
    void mark(uint32_t page, uint32_t count, bool used);

    // One bit per page; 512 pages -> 16 words.
    uint32_t m_bitmap[kPageCount / 32] = {};
    Record m_records[kMaxAllocs] = {};
    uint32_t m_used_pages = 0;

    // CLUT slot pages. Eight 1 KB slots per page, tracked as a byte of bits
    // so a slot can be freed and reused without disturbing its neighbours.
    static constexpr uint32_t kClutSlotsPerPage = 8;
    static constexpr uint32_t kMaxClutPages = 8; // 64 CLUTs, the texture cap
    uint32_t m_clut_pages[kMaxClutPages] = {};
    uint8_t m_clut_used[kMaxClutPages] = {};
    uint32_t m_clut_page_count = 0;
};

} // namespace gfx
} // namespace ps2ur
