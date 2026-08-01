#include "ps2ur/gs_texcache.h"

#include "ps2ur/assert.h"
#include "ps2ur/log.h"

#include <cstdio>

namespace ps2ur {
namespace gfx {

namespace {
constexpr unsigned u(uint32_t v) { return static_cast<unsigned>(v); }

// A 256-entry CLUT is 1 KB, which still costs a whole 8 KB page because
// FRAME/TEX base pointers are page- and block-aligned. Worth knowing when the
// budget looks mysteriously small with many indexed textures.
constexpr uint32_t kClutPages = 1;
} // namespace

void TextureCache::init(uint32_t budget_pages)
{
    m_budget_pages = budget_pages;
    m_used_pages = 0;
    m_count = 0;
    m_clock = 0;
    m_stats = Stats{};
    for (uint32_t i = 0; i < kMaxTextures; ++i) {
        m_entries[i] = Entry{};
    }
}

void TextureCache::shutdown(GsDevice& device)
{
    for (uint32_t i = 0; i < m_count; ++i) {
        Entry& e = m_entries[i];
        if (e.alloc.valid()) {
            device.vram().free(e.alloc);
        }
        if (e.clut_alloc.valid()) {
            device.vram().free(e.clut_alloc);
        }
        e = Entry{};
    }
    m_count = 0;
    m_used_pages = 0;
}

uint32_t TextureCache::pages_needed(const Entry& entry) const
{
    uint32_t pages = pages_for_buffer(entry.w, entry.h, entry.fmt);
    if (entry.clut != nullptr) {
        pages += kClutPages;
    }
    return pages;
}

TextureCache::Handle TextureCache::add(const void* pixels, uint32_t w, uint32_t h,
                                       PixelFormat fmt, const uint32_t* clut,
                                       uint32_t clut_entries, const char* name)
{
    if (pixels == nullptr || w == 0 || h == 0) {
        return kInvalidHandle;
    }
    if (m_count >= kMaxTextures) {
        log(LogLevel::Error, "texcache: full (%u textures)", u(kMaxTextures));
        return kInvalidHandle;
    }

    Entry& e = m_entries[m_count];
    e = Entry{};
    e.pixels = pixels;
    e.clut = clut;
    e.clut_entries = clut_entries;
    e.w = w;
    e.h = h;
    e.fmt = fmt;
    e.name = name;
    e.used = true;

    // A texture larger than the whole budget can never be made resident, and
    // finding that out at bind time in the middle of a frame is worse than
    // finding it out now.
    const uint32_t need = pages_needed(e);
    if (need > m_budget_pages) {
        log(LogLevel::Error,
            "texcache: '%s' needs %u pages but the budget is only %u",
            name != nullptr ? name : "?", u(need), u(m_budget_pages));
        e = Entry{};
        return kInvalidHandle;
    }

    return m_count++;
}

void TextureCache::set_pinned(Handle handle, bool pinned)
{
    if (handle >= m_count) {
        return;
    }
    m_entries[handle].pinned = pinned;
}

void TextureCache::begin_frame()
{
    if (m_stats.uploads_this_frame > m_stats.peak_uploads_per_frame) {
        m_stats.peak_uploads_per_frame = m_stats.uploads_this_frame;
    }
    m_stats.uploads_this_frame = 0;
    m_stats.evictions_this_frame = 0;
}

bool TextureCache::evict_one(GsDevice& device)
{
    // Least recently used, skipping pinned entries.
    uint32_t victim = kMaxTextures;
    uint64_t oldest = UINT64_MAX;
    for (uint32_t i = 0; i < m_count; ++i) {
        Entry& e = m_entries[i];
        if (!e.used || !e.alloc.valid() || e.pinned) {
            continue;
        }
        if (e.last_used < oldest) {
            oldest = e.last_used;
            victim = i;
        }
    }
    if (victim == kMaxTextures) {
        return false; // everything resident is pinned
    }

    Entry& e = m_entries[victim];
    const uint32_t freed = pages_needed(e);
    device.vram().free(e.alloc);
    e.alloc = VramAlloc{};
    if (e.clut_alloc.valid()) {
        device.vram().free(e.clut_alloc);
        e.clut_alloc = VramAlloc{};
    }
    m_used_pages -= freed;
    m_stats.resident--;
    m_stats.resident_pages -= freed;
    m_stats.evictions_this_frame++;
    return true;
}

bool TextureCache::make_resident(GsDevice& device, Entry& entry)
{
    const uint32_t need = pages_needed(entry);

    // Evict until there is both budget headroom and a contiguous run. The two
    // are different: the allocator can report enough free pages while
    // fragmentation leaves no run long enough.
    for (;;) {
        if (m_used_pages + need <= m_budget_pages) {
            VramAlloc tex =
                device.vram().alloc_buffer(entry.w, entry.h, entry.fmt, entry.name);
            if (tex.valid()) {
                VramAlloc clut;
                if (entry.clut != nullptr) {
                    clut = device.vram().alloc_buffer(16, 16, PixelFormat::PSMCT32,
                                                      "clut");
                    if (!clut.valid()) {
                        device.vram().free(tex);
                        if (!evict_one(device)) {
                            m_stats.failed_binds++;
                            return false;
                        }
                        continue;
                    }
                }
                entry.alloc = tex;
                entry.clut_alloc = clut;
                break;
            }
        }
        if (!evict_one(device)) {
            m_stats.failed_binds++;
            log(LogLevel::Warn,
                "texcache: cannot fit '%s' (%u pages); %u/%u pages used, all "
                "remaining entries pinned",
                entry.name != nullptr ? entry.name : "?", u(need), u(m_used_pages),
                u(m_budget_pages));
            return false;
        }
    }

    if (!device.upload_texture(entry.pixels, entry.alloc, entry.w, entry.h, entry.fmt)) {
        device.vram().free(entry.alloc);
        entry.alloc = VramAlloc{};
        if (entry.clut_alloc.valid()) {
            device.vram().free(entry.clut_alloc);
            entry.clut_alloc = VramAlloc{};
        }
        m_stats.failed_binds++;
        return false;
    }
    if (entry.clut != nullptr &&
        !device.upload_clut(entry.clut, entry.clut_alloc, entry.clut_entries)) {
        m_stats.failed_binds++;
        return false;
    }

    m_used_pages += need;
    m_stats.resident++;
    m_stats.resident_pages += need;
    m_stats.uploads_this_frame++;
    return true;
}

bool TextureCache::bind(GsDevice& device, Handle handle)
{
    if (handle >= m_count) {
        return false;
    }
    Entry& e = m_entries[handle];
    if (!e.used) {
        return false;
    }

    if (e.alloc.valid()) {
        m_stats.hits++;
    } else {
        m_stats.misses++;
        if (!make_resident(device, e)) {
            return false;
        }
    }

    e.last_used = ++m_clock;

    if (e.clut != nullptr) {
        device.set_texture_indexed(e.alloc, e.w, e.h, e.fmt, e.clut_alloc,
                                   e.clut_entries);
    } else {
        device.set_texture(e.alloc, e.w, e.h, e.fmt);
    }
    return true;
}

void TextureCache::format_stats(char* buffer, uint32_t size) const
{
    if (buffer == nullptr || size == 0) {
        return;
    }
    std::snprintf(buffer, size, "TEX %u/%u pg  UP %u  EV %u  HIT %u  MISS %u",
                  u(m_stats.resident_pages), u(m_budget_pages),
                  u(m_stats.uploads_this_frame), u(m_stats.evictions_this_frame),
                  u(m_stats.hits), u(m_stats.misses));
}

} // namespace gfx
} // namespace ps2ur
