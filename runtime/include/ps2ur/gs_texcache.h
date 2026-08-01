// Texture residency cache (plan section 9, M3 task 2).
//
// VRAM is 4 MB and the framebuffers already eat ~2.7 MB of it, so the texture
// pool is around 1.2 MB in the baseline config -- roughly nineteen 256x256
// PSMT8 textures. Any real scene has more than that, so textures must come and
// go, and something has to decide which.
//
// This is an LRU over VramAllocator with pinning for the things that must
// never leave (the debug font, UI atlases). It also counts uploads per frame,
// because thrashing is the failure mode that quietly eats a frame budget:
// twenty uploads a frame will hold 30 fps and forty will not, and the only way
// to notice is to measure it.
//
// Source pixels stay in main RAM; a miss re-uploads from there. That is the
// right trade on this machine -- main RAM is 32 MB and VRAM is 4 MB.
//
// SIZING WARNING: every miss appends its whole image payload to the frame
// packet (a 128x64 PSMT8 texture is 512 qwords, a 256x256 PSMCT32 one is
// 16384). A thrashing cache can therefore overflow VideoConfig::packet_qwords,
// which surfaces as bind() returning false rather than as a dropped upload.
// Size the packet for the worst-case number of misses in a frame, and watch
// uploads_this_frame -- if it is large enough to threaten the packet, the
// budget is too small for the working set and the real fix is fewer or smaller
// textures, not a bigger packet.
#pragma once

#include "ps2ur/gs_device.h"
#include "ps2ur/gs_format.h"
#include "ps2ur/gs_vram.h"

#include <cstdint>

namespace ps2ur {
namespace gfx {

class TextureCache {
public:
    using Handle = uint32_t;
    static constexpr Handle kInvalidHandle = 0xFFFFFFFFu;
    static constexpr uint32_t kMaxTextures = 128;

    struct Stats {
        uint32_t resident = 0;          // textures currently in VRAM
        uint32_t resident_pages = 0;
        uint32_t hits = 0;              // binds that were already resident
        uint32_t misses = 0;            // binds that required an upload
        uint32_t uploads_this_frame = 0;
        uint32_t evictions_this_frame = 0;
        uint32_t peak_uploads_per_frame = 0;
        uint32_t failed_binds = 0;      // could not fit even after evicting
    };

    void init(uint32_t budget_pages);
    void shutdown(GsDevice& device);

    // Describes a texture without uploading it. 'pixels' and 'clut' must
    // outlive the cache -- they are the copy a miss re-uploads from.
    // 'clut' may be null for non-indexed formats, and must ALREADY be in CSM1
    // storage order (see clut_csm1_reorder).
    Handle add(const void* pixels, uint32_t w, uint32_t h, PixelFormat fmt,
               const uint32_t* clut, uint32_t clut_entries, const char* name);

    // Makes the texture resident (uploading and evicting as needed) and binds
    // it for subsequent draws. Returns false only if it cannot be made to fit,
    // which means the budget is genuinely too small for this texture.
    bool bind(GsDevice& device, Handle handle);

    // Pinned textures are never evicted. Use sparingly -- every pinned page is
    // a page the LRU cannot work with.
    void set_pinned(Handle handle, bool pinned);

    // Call once per frame before any bind(), so per-frame counters mean
    // something.
    void begin_frame();

    const Stats& stats() const { return m_stats; }

    // One-line summary for the debug overlay.
    void format_stats(char* buffer, uint32_t size) const;

private:
    struct Entry {
        const void* pixels = nullptr;
        const uint32_t* clut = nullptr;
        uint32_t clut_entries = 0;
        uint32_t w = 0;
        uint32_t h = 0;
        PixelFormat fmt = PixelFormat::PSMCT32;
        const char* name = nullptr;

        VramAlloc alloc;      // valid when resident
        VramAlloc clut_alloc; // valid when resident and indexed
        uint64_t last_used = 0;
        bool pinned = false;
        bool used = false;
    };

    bool make_resident(GsDevice& device, Entry& entry);
    bool evict_one(GsDevice& device);
    uint32_t pages_needed(const Entry& entry) const;

    VramAllocator* m_vram = nullptr;
    Entry m_entries[kMaxTextures];
    uint32_t m_count = 0;
    uint32_t m_budget_pages = 0;
    uint32_t m_used_pages = 0;
    uint64_t m_clock = 0;
    Stats m_stats;
};

} // namespace gfx
} // namespace ps2ur
