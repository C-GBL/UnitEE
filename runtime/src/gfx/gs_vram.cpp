#include "ps2ur/gs_vram.h"

#include "ps2ur/assert.h"
#include "ps2ur/log.h"

#include <cstring>

namespace ps2ur {
namespace gfx {

namespace {
// On the EE, uint32_t is `unsigned long`; on the x86-64 host it is
// `unsigned int`. printf's %u wants `unsigned int` on both.
constexpr unsigned u(uint32_t v) { return static_cast<unsigned>(v); }
} // namespace

void VramAllocator::init()
{
    reset();
}

void VramAllocator::reset()
{
    for (uint32_t i = 0; i < kPageCount / 32; ++i) {
        m_bitmap[i] = 0;
    }
    for (uint32_t i = 0; i < kMaxAllocs; ++i) {
        m_records[i] = Record{};
    }
    m_used_pages = 0;
    for (uint32_t i = 0; i < kMaxClutPages; ++i) {
        m_clut_pages[i] = 0;
        m_clut_used[i] = 0;
    }
    m_clut_page_count = 0;
}

bool VramAllocator::is_free(uint32_t page, uint32_t count) const
{
    if (count == 0 || page + count > kPageCount) {
        return false;
    }
    for (uint32_t i = page; i < page + count; ++i) {
        if ((m_bitmap[i / 32] >> (i % 32)) & 1u) {
            return false;
        }
    }
    return true;
}

void VramAllocator::mark(uint32_t page, uint32_t count, bool used)
{
    for (uint32_t i = page; i < page + count; ++i) {
        const uint32_t word = i / 32;
        const uint32_t bit = 1u << (i % 32);
        if (used) {
            m_bitmap[word] |= bit;
        } else {
            m_bitmap[word] &= ~bit;
        }
    }
    m_used_pages += used ? count : -static_cast<int32_t>(count);
}

VramAlloc VramAllocator::alloc_pages(uint32_t count, const char* debug_name)
{
    VramAlloc result;
    if (count == 0 || count > kPageCount) {
        return result;
    }

    // First fit. With a handful of long-lived buffers plus texture churn this
    // beats best-fit in practice and is far easier to reason about; if
    // fragmentation ever bites, M3's cache is the place to add compaction.
    uint32_t page = kPageCount;
    for (uint32_t i = 0; i + count <= kPageCount; ++i) {
        if (is_free(i, count)) {
            page = i;
            break;
        }
    }
    if (page == kPageCount) {
        log(LogLevel::Warn,
            "vram: out of space for '%s' (%u pages, %u free, largest run %u)",
            debug_name != nullptr ? debug_name : "?", u(count), u(free_pages()),
            u(largest_free_run()));
        return result;
    }

    uint32_t slot = kMaxAllocs;
    for (uint32_t i = 0; i < kMaxAllocs; ++i) {
        if (!m_records[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot == kMaxAllocs) {
        log(LogLevel::Warn, "vram: reservation table full (%u entries)", u(kMaxAllocs));
        return result;
    }

    mark(page, count, true);
    m_records[slot].page = page;
    m_records[slot].page_count = count;
    m_records[slot].name = debug_name;
    m_records[slot].in_use = true;

    result.page = page;
    result.page_count = count;
    return result;
}

VramAlloc VramAllocator::alloc_buffer(uint32_t width, uint32_t height,
                                      PixelFormat fmt, const char* debug_name)
{
    return alloc_pages(pages_for_buffer(width, height, fmt), debug_name);
}

void VramAllocator::log_budget() const
{
    // Framebuffer-ish reservations are the ones made before any texture: the
    // colour buffers and the Z buffer. Rather than guess from names, split by
    // what the caller told us at allocation time.
    uint32_t frame_pages = 0;
    uint32_t texture_pages = 0;
    for (uint32_t i = 0; i < kMaxAllocs; ++i) {
        const Record& r = m_records[i];
        if (!r.in_use) {
            continue;
        }
        const char* n = r.name != nullptr ? r.name : "";
        // The device names these three; anything else is content.
        const bool is_frame = std::strncmp(n, "colour", 6) == 0 ||
                              std::strncmp(n, "depth", 5) == 0;
        if (is_frame) {
            frame_pages += r.page_count;
        } else {
            texture_pages += r.page_count;
        }
    }
    const uint32_t kb = kPageBytes / 1024u;
    log(LogLevel::Info,
        "[vram] %u KB total: framebuffer %u KB, textures %u KB, free %u KB",
        u(kPageCount * kb), u(frame_pages * kb), u(texture_pages * kb),
        u(free_pages() * kb));
    // The CLUT line exists because packing them is worth 7 KB per texture,
    // and a number nobody can see is a number nobody maintains.
    const uint32_t slots = clut_slots_used();
    log(LogLevel::Info,
        "[vram] %u CLUTs in %u pages (%u KB); one page each would have cost "
        "%u KB",
        u(slots), u(m_clut_page_count), u(m_clut_page_count * kb),
        u(slots * kb));
}

VramAlloc VramAllocator::alloc_clut(const char* debug_name)
{
    VramAlloc result;
    // A free slot in a page already reserved for CLUTs.
    for (uint32_t p = 0; p < m_clut_page_count; ++p) {
        for (uint32_t s = 0; s < kClutSlotsPerPage; ++s) {
            if ((m_clut_used[p] & (1u << s)) == 0u) {
                m_clut_used[p] |= static_cast<uint8_t>(1u << s);
                result.page = m_clut_pages[p];
                result.page_count = 0; // a slot, not a run of pages
                result.block_in_page = s * (kBlocksPerPage / kClutSlotsPerPage);
                return result;
            }
        }
    }
    // None free: reserve another page to carve up.
    if (m_clut_page_count >= kMaxClutPages) {
        log(LogLevel::Warn, "vram: out of CLUT slots for '%s' (%u pages used)",
            debug_name != nullptr ? debug_name : "?", u(m_clut_page_count));
        return result;
    }
    const VramAlloc page = alloc_pages(1, "clut-page");
    if (!page.valid()) {
        return result;
    }
    const uint32_t index = m_clut_page_count++;
    m_clut_pages[index] = page.page;
    m_clut_used[index] = 1u; // slot 0 goes to this caller
    result.page = page.page;
    result.page_count = 0;
    result.block_in_page = 0;
    return result;
}

uint32_t VramAllocator::clut_slots_used() const
{
    uint32_t used = 0;
    for (uint32_t p = 0; p < m_clut_page_count; ++p) {
        for (uint32_t s = 0; s < kClutSlotsPerPage; ++s) {
            if ((m_clut_used[p] & (1u << s)) != 0u) {
                ++used;
            }
        }
    }
    return used;
}

void VramAllocator::free(const VramAlloc& alloc)
{
    if (!alloc.valid()) {
        return;
    }
    // A CLUT slot: release the slot, keep the page. The page stays because a
    // scene swap frees every CLUT and then immediately allocates the same
    // number again; returning the pages would just fragment the pool.
    if (alloc.is_slot()) {
        const uint32_t slot =
            alloc.block_in_page / (kBlocksPerPage / kClutSlotsPerPage);
        for (uint32_t p = 0; p < m_clut_page_count; ++p) {
            if (m_clut_pages[p] == alloc.page && slot < kClutSlotsPerPage) {
                m_clut_used[p] &= static_cast<uint8_t>(~(1u << slot));
                return;
            }
        }
        return;
    }
    if (alloc.page_count == 0) {
        return;
    }
    for (uint32_t i = 0; i < kMaxAllocs; ++i) {
        Record& r = m_records[i];
        if (r.in_use && r.page == alloc.page && r.page_count == alloc.page_count) {
            mark(r.page, r.page_count, false);
            r = Record{};
            return;
        }
    }
    // Freeing something this allocator never handed out means the caller is
    // confused about ownership; that is worth catching loudly in debug.
    PS2UR_ASSERT(false && "vram: free() of an unknown reservation");
}

uint32_t VramAllocator::largest_free_run() const
{
    uint32_t best = 0;
    uint32_t run = 0;
    for (uint32_t i = 0; i < kPageCount; ++i) {
        if ((m_bitmap[i / 32] >> (i % 32)) & 1u) {
            run = 0;
        } else {
            run++;
            if (run > best) {
                best = run;
            }
        }
    }
    return best;
}

void VramAllocator::debug_dump() const
{
    log(LogLevel::Info, "vram: %u/%u pages used (%u KB / %u KB), largest free run %u",
        u(m_used_pages), u(kPageCount), u(m_used_pages * kPageBytes / 1024u),
        u(kVramBytes / 1024u), u(largest_free_run()));

    for (uint32_t i = 0; i < kMaxAllocs; ++i) {
        const Record& r = m_records[i];
        if (!r.in_use) {
            continue;
        }
        log(LogLevel::Info, "  page %3u..%3u  %4u KB  block %5u  %s",
            u(r.page), u(r.page + r.page_count - 1u),
            u(r.page_count * kPageBytes / 1024u),
            u(r.page * kBlocksPerPage), r.name != nullptr ? r.name : "?");
    }

    // Occupancy map: one character per 8 pages (64 KB), 64 characters total.
    char map[kPageCount / 8 + 1];
    for (uint32_t g = 0; g < kPageCount / 8; ++g) {
        uint32_t occupied = 0;
        for (uint32_t p = g * 8; p < g * 8 + 8; ++p) {
            occupied += (m_bitmap[p / 32] >> (p % 32)) & 1u;
        }
        map[g] = (occupied == 0) ? '.' : (occupied == 8) ? '#' : '+';
    }
    map[kPageCount / 8] = '\0';
    log(LogLevel::Info, "  [%s]  ('.' free  '+' partial  '#' full, 64 KB each)", map);
}

} // namespace gfx
} // namespace ps2ur
