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

    static constexpr uint32_t kInvalidPage = 0xFFFFFFFFu;

    bool valid() const { return page != kInvalidPage; }

    // TEX0.TBP0 is in 256-byte blocks.
    uint32_t block() const { return page * kBlocksPerPage; }
    uint32_t byte_offset() const { return page * kPageBytes; }
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

    void free(const VramAlloc& alloc);

    uint32_t used_pages() const { return m_used_pages; }
    uint32_t free_pages() const { return kPageCount - m_used_pages; }
    uint32_t largest_free_run() const;

    // Prints the reservation table and a page-occupancy map to the log.
    // Required by M2 task 3; this is the tool you reach for when a texture
    // upload lands on top of the Z buffer.
    void debug_dump() const;

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
};

} // namespace gfx
} // namespace ps2ur
