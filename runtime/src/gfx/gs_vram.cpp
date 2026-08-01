#include "ps2ur/gs_vram.h"

#include "ps2ur/assert.h"
#include "ps2ur/log.h"

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

void VramAllocator::free(const VramAlloc& alloc)
{
    if (!alloc.valid() || alloc.page_count == 0) {
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
