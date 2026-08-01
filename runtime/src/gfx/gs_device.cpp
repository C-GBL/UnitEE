#include "ps2ur/gs_device.h"

#include "ps2ur/alloc.h"
#include "ps2ur/assert.h"
#include "ps2ur/log.h"

#if defined(PS2UR_PLATFORM_PS2)
#include <dma.h>
#include <draw.h>
#include <graph.h>
#include <kernel.h>
#endif

namespace ps2ur {
namespace gfx {

namespace {

// On the EE, uint32_t is `unsigned long`, while on the x86-64 host build it is
// `unsigned int`. printf's %u wants `unsigned int` on both, so every logged
// uint32_t goes through this. Without it the dual-target build fails on one
// side or the other -- which is precisely what M1's host target is for.
constexpr unsigned u(uint32_t v) { return static_cast<unsigned>(v); }

// GS TEST register ZTST values. (0 = never, 3 = greater; neither is used yet.)
constexpr uint32_t kZTestAlways = 1;
constexpr uint32_t kZTestGEqual = 2;

} // namespace

uint32_t GsDevice::draw_buffer_page() const
{
    return m_colour[m_frame_index & 1u].page;
}

uint32_t GsDevice::display_buffer_page() const
{
    return m_colour[(m_frame_index + 1u) & 1u].page;
}

bool GsDevice::init(const VideoConfig& config)
{
    if (m_initialized) {
        return true;
    }
    m_config = config;
    m_vram.init();

    // Two colour buffers plus one Z buffer, reserved for the lifetime of the
    // device. Page-aligned by construction, which FRAME.FBP/ZBUF.ZBP require.
    m_colour[0] = m_vram.alloc_buffer(config.width, config.height,
                                      config.colour_format, "colour0");
    m_colour[1] = m_vram.alloc_buffer(config.width, config.height,
                                      config.colour_format, "colour1");
    if (!m_colour[0].valid() || !m_colour[1].valid()) {
        log(LogLevel::Error, "gfx: no VRAM for colour buffers (%ux%u)",
            u(config.width), u(config.height));
        return false;
    }
    if (config.depth_enabled) {
        m_depth = m_vram.alloc_buffer(config.width, config.height,
                                      config.depth_format, "depth");
        if (!m_depth.valid()) {
            log(LogLevel::Error, "gfx: no VRAM for the depth buffer");
            return false;
        }
    }

    m_packet_memory =
        static_cast<Qword*>(heap_alloc(config.packet_qwords * sizeof(Qword), 16));
    if (m_packet_memory == nullptr) {
        log(LogLevel::Error, "gfx: could not allocate a %u-qword packet buffer",
            u(config.packet_qwords));
        return false;
    }
    m_packet.init(m_packet_memory, config.packet_qwords);

    m_frame_index = 0;

#if defined(PS2UR_PLATFORM_PS2)
    dma_channel_initialize(DMA_CHANNEL_GIF, nullptr, 0);
    // Arm the channel for dma_wait_fast(). This pairing -- fast_waits at init,
    // dma_wait_fast() around every send -- is the combination ps2sdk's own
    // samples use and is known to work. dma_channel_wait(ch, -1) was tried
    // first and is NOT a substitute: its timeout semantics are undocumented
    // and a negative value appears not to wait at all, which let the EE
    // overwrite packet memory while the DMAC was still reading it.
    dma_channel_fast_waits(DMA_CHANNEL_GIF);

    // libgraph owns the CRTC. graph_initialize() picks the region-appropriate
    // timings and ties the read circuit to our first colour buffer; FBP is a
    // page index, which is exactly what VramAlloc::page already is.
    graph_initialize(static_cast<int>(m_colour[0].page),
                     static_cast<int>(config.width), static_cast<int>(config.height),
                     static_cast<int>(config.colour_format), 0, 0);
#endif

    m_initialized = true;
    log(LogLevel::Info, "gfx: %ux%u %s %s, colour pages %u/%u, depth page %u",
        u(config.width), u(config.height),
        config.standard == VideoStandard::NTSC ? "NTSC" : "PAL",
        config.interlaced ? "interlaced" : "progressive",
        u(m_colour[0].page), u(m_colour[1].page),
        u(config.depth_enabled ? m_depth.page : 0u));
    m_vram.debug_dump();

    // Establish the drawing environment once; end_frame() re-points FRAME_1 at
    // the new back buffer on every flip.
    build_drawing_environment();
    return true;
}

void GsDevice::submit_and_wait()
{
#if defined(PS2UR_PLATFORM_PS2)
    if (m_packet.size() == 0) {
        return;
    }
    // Synchronous submit: issue the transfer, then wait for it to drain before
    // returning, so the caller can freely rebuild the packet. M4 replaces this
    // with double-buffered DMA chains so the EE builds frame N+1 while the
    // DMAC walks frame N (plan section 3.4).
    //
    // There is deliberately NO wait before the send. dma_wait_fast() waits on
    // channel status that only becomes meaningful once a transfer has actually
    // been issued; calling it on a never-used channel hangs forever. That cost
    // a bring-up session -- the first submit stalled before the GS had ever
    // seen a byte, which presents as a black screen rather than as a hang you
    // can locate.
    if (m_trace) {
        log(LogLevel::Debug, "gfx.trace: submit %u qwords", u(m_packet.size()));
    }

    // MANDATORY: the EE wrote this packet through its data cache, but the DMAC
    // reads physical memory and knows nothing about that cache. Without a
    // writeback the GIF consumes whatever stale bytes are in RAM and rasterises
    // them as primitives -- which looks like random lines and scattered
    // geometry on screen, not like a clean failure.
    //
    // The alternative is allocating the packet in uncached (UCAB) memory, which
    // avoids the flush but makes every EE write slow. Flushing once per submit
    // is the better trade while packets are built in main RAM; when M4 moves
    // chain assembly into the scratchpad this goes away, since the SPR is not
    // cached.
    FlushCache(0);
    if (m_trace) {
        log(LogLevel::Debug, "gfx.trace: flushed, sending");
    }
    dma_channel_send_normal(DMA_CHANNEL_GIF, m_packet.data(),
                            static_cast<int>(m_packet.size()), 0, 0);
    if (m_trace) {
        log(LogLevel::Debug, "gfx.trace: sent, post-wait");
    }
    // Wait for the transfer to drain before the caller reuses the packet.
    dma_wait_fast();
    if (m_trace) {
        log(LogLevel::Debug, "gfx.trace: post-wait done");
    }
#endif
}

void GsDevice::flush_packet()
{
    if (!m_initialized || m_packet.size() == 0) {
        m_packet.reset();
        return;
    }
    m_packet.set_last_tag_eop();
    submit_and_wait();
    m_packet.reset();
}

void GsDevice::build_drawing_environment()
{
    m_packet.reset();

    // Six state registers: where to draw, where the Z buffer is, the screen
    // origin, the scissor rectangle, the depth test, and colour clamping.
    m_packet.begin_packed_ad(6);
    m_packet.add_ad(GsReg::FRAME_1,
                    gs_frame(draw_buffer_page(), buffer_width_units(m_config.width),
                             m_config.colour_format, 0));
    m_packet.add_ad(GsReg::ZBUF_1,
                    gs_zbuf(m_config.depth_enabled ? m_depth.page : 0,
                            m_config.depth_format, !m_config.depth_enabled));
    // Screen (0,0) maps to the GS origin at 2048; see kGsOriginX.
    m_packet.add_ad(GsReg::XYOFFSET_1, gs_xyoffset(kGsOriginX << 4, kGsOriginY << 4));
    m_packet.add_ad(GsReg::SCISSOR_1,
                    gs_scissor(0, m_config.width - 1u, 0, m_config.height - 1u));
    m_packet.add_ad(GsReg::TEST_1,
                    gs_test(false, 0, 0, 0, false, 0, m_config.depth_enabled,
                            m_config.depth_enabled ? kZTestGEqual : kZTestAlways));
    // Clamp rather than wrap colour arithmetic; wrapping produces the classic
    // psychedelic overflow artefacts on additive blends.
    m_packet.add_ad(GsReg::COLCLAMP, 1);
    m_packet.set_last_tag_eop();

    // Building the environment is not enough -- it has to reach the GS before
    // the first primitive, or XYOFFSET and SCISSOR keep their power-on values
    // and nothing lands where you expect.
    submit_and_wait();
    m_packet.reset();
}

void GsDevice::begin_frame()
{
    PS2UR_ASSERT(m_initialized);
    m_packet.reset();

    // FRAME_1 must follow the buffer we are drawing into this frame.
    m_packet.begin_packed_ad(1);
    m_packet.add_ad(GsReg::FRAME_1,
                    gs_frame(draw_buffer_page(), buffer_width_units(m_config.width),
                             m_config.colour_format, 0));
}

void GsDevice::clear(uint8_t r, uint8_t g, uint8_t b, uint32_t depth)
{
    PS2UR_ASSERT(m_initialized);

    // A clear is just a full-screen sprite. The only subtlety is the depth
    // test: it must be forced to ALWAYS with Z-write on, otherwise the clear
    // is itself depth-rejected against last frame's Z values and leaves them
    // in place -- which shows up as geometry from the previous frame
    // mysteriously occluding this one.
    m_packet.begin_packed_ad(2);
    m_packet.add_ad(GsReg::TEST_1, gs_test(false, 0, 0, 0, false, 0, true, kZTestAlways));
    m_packet.add_ad(GsReg::PRMODECONT, 1); // take PRIM from the GIFtag

    const uint64_t prim = gs_prim(GsPrim::Sprite, false, false, false, false,
                                  false, false, 0, false);
    // PACKED-mode data: gs_packed_*, never the native gs_rgbaq/gs_xyz forms.
    m_packet.begin_packed(1, 3, gs_reglist(GsReg::RGBAQ, GsReg::XYZ2, GsReg::XYZ2),
                          false, true, prim);
    m_packet.add_qword(gs_packed_rgbaq(r, g, b, 0x80));
    m_packet.add_qword(gs_packed_xyz(gs_coord(0), gs_coord(0), depth));
    m_packet.add_qword(gs_packed_xyz(gs_coord(static_cast<int32_t>(m_config.width)),
                                     gs_coord(static_cast<int32_t>(m_config.height)),
                                     depth));

    // Restore the normal depth test for subsequent geometry.
    m_packet.begin_packed_ad(1);
    m_packet.add_ad(GsReg::TEST_1,
                    gs_test(false, 0, 0, 0, false, 0, m_config.depth_enabled,
                            m_config.depth_enabled ? kZTestGEqual : kZTestAlways));
}

void GsDevice::draw_triangles_immediate(const Vertex* vertices, uint32_t count)
{
    PS2UR_ASSERT(m_initialized);
    if (vertices == nullptr || count < 3) {
        return;
    }
    const uint32_t triangles = count / 3u;

    m_packet.begin_packed_ad(1);
    m_packet.add_ad(GsReg::PRMODECONT, 1);

    // Gouraud-shaded, untextured triangles: RGBAQ + XYZ2 per vertex.
    const uint64_t prim = gs_prim(GsPrim::Triangle, true, false, false, false,
                                  false, false, 0, false);
    m_packet.begin_packed(triangles * 3u, 2, gs_reglist(GsReg::RGBAQ, GsReg::XYZ2),
                          false, true, prim);
    for (uint32_t i = 0; i < triangles * 3u; ++i) {
        const Vertex& v = vertices[i];
        m_packet.add_qword(gs_packed_rgbaq(v.r, v.g, v.b, v.a));
        m_packet.add_qword(gs_packed_xyz(gs_coord(v.x), gs_coord(v.y), v.z));
    }
}

void GsDevice::end_frame()
{
    PS2UR_ASSERT(m_initialized);

    m_packet.set_last_tag_eop();

    if (m_packet.overflowed()) {
        // Submitting a truncated packet would leave the GIF waiting for data
        // that never arrives and wedge the DMAC. Drop the frame instead and
        // say so loudly.
        log(LogLevel::Error,
            "gfx: frame packet overflowed (%u qword capacity); frame dropped. "
            "Raise VideoConfig::packet_qwords.",
            u(m_packet.capacity()));
        m_packet.reset();
        return;
    }

#if defined(PS2UR_PLATFORM_PS2)
    submit_and_wait();
    // Display the buffer we just finished drawing. The vsync wait both paces
    // the loop and ensures the flip happens between fields rather than mid-scan.
    graph_wait_vsync();
    graph_set_framebuffer_filtered(static_cast<int>(draw_buffer_page()),
                                   static_cast<int>(m_config.width),
                                   static_cast<int>(m_config.colour_format), 0, 0);
#endif

    m_frame_index++;
}

bool GsDevice::upload_texture(const void* data, const VramAlloc& dest, uint32_t w,
                              uint32_t h, PixelFormat fmt)
{
    PS2UR_ASSERT(m_initialized);
    if (data == nullptr || !dest.valid() || w == 0 || h == 0) {
        return false;
    }

    const uint32_t bytes = (w * h * bits_per_pixel(fmt)) / 8u;
    if ((bytes & 15u) != 0) {
        log(LogLevel::Error, "gfx: texture payload must be a whole number of qwords");
        return false;
    }
    const uint32_t qwords = bytes / 16u;

    // Describe the destination, then hand the GIF the raw pixels in IMAGE
    // mode. TRXDIR is written last because it arms the transfer.
    m_packet.begin_packed_ad(4);
    m_packet.add_ad(GsReg::BITBLTBUF,
                    gs_bitbltbuf(0, 0, PixelFormat::PSMCT32,
                                 dest.block(), buffer_width_units(w), fmt));
    m_packet.add_ad(GsReg::TRXPOS, gs_trxpos(0, 0, 0, 0, 0));
    m_packet.add_ad(GsReg::TRXREG, gs_trxreg(w, h));
    m_packet.add_ad(GsReg::TRXDIR, 0); // 0 = host -> local

    m_packet.begin_image(qwords);
    const Qword* src = static_cast<const Qword*>(data);
    for (uint32_t i = 0; i < qwords; ++i) {
        m_packet.add_qword(src[i].lo, src[i].hi);
    }

    // The GS caches texels; without a flush a re-upload to the same address
    // can be drawn with the previous contents.
    m_packet.begin_packed_ad(1);
    m_packet.add_ad(GsReg::TEXFLUSH, 0);

    return !m_packet.overflowed();
}

void GsDevice::set_texture(const VramAlloc& tex, uint32_t w, uint32_t h, PixelFormat fmt)
{
    PS2UR_ASSERT(m_initialized);
    m_packet.begin_packed_ad(3);
    // MODULATE so vertex colour still lights the texel, which is what the
    // fixed material model in plan section 7.3 expects of Unlit/VertexLit.
    m_packet.add_ad(GsReg::TEX0_1,
                    gs_tex0(tex.block(), buffer_width_units(w), fmt, log2_pot(w),
                            log2_pot(h), false, /*MODULATE*/ 0, 0, 0, 0, 0, 0));
    m_packet.add_ad(GsReg::TEX1_1, gs_tex1_nearest());
    m_packet.add_ad(GsReg::TEXA, gs_texa(0x80, false, 0x80));
}

bool GsDevice::upload_clut(const uint32_t* palette, const VramAlloc& dest,
                           uint32_t entries)
{
    if (palette == nullptr || !dest.valid()) {
        return false;
    }
    // The CLUT is uploaded as an ordinary PSMCT32 image; only its shape is
    // fixed by the entry count.
    uint32_t w = 16u, h = 16u; // 256 entries
    if (entries == 16u) {
        w = 8u;
        h = 2u;
    } else if (entries != 256u) {
        log(LogLevel::Error, "gfx: CLUT must have 16 or 256 entries, got %u", u(entries));
        return false;
    }
    return upload_texture(palette, dest, w, h, PixelFormat::PSMCT32);
}

void GsDevice::set_texture_indexed(const VramAlloc& tex, uint32_t w, uint32_t h,
                                   PixelFormat fmt, const VramAlloc& clut,
                                   uint32_t clut_entries)
{
    PS2UR_ASSERT(m_initialized);
    (void)clut_entries;

    m_packet.begin_packed_ad(3);
    // CLD=1 loads the CLUT from CBP into the GS's palette cache on this draw.
    // CSM=0 is CSM1 (the mode whose 32-entry block shuffle the exporter must
    // pre-apply); CPSM=PSMCT32; CSA=0 (no entry offset).
    m_packet.add_ad(GsReg::TEX0_1,
                    gs_tex0(tex.block(), buffer_width_units(w), fmt, log2_pot(w),
                            log2_pot(h), false, /*MODULATE*/ 0,
                            clut.block(), static_cast<uint32_t>(PixelFormat::PSMCT32) & 0xFu,
                            /*CSM1*/ 0, /*CSA*/ 0, /*CLD=load*/ 1));
    m_packet.add_ad(GsReg::TEX1_1, gs_tex1_nearest());
    m_packet.add_ad(GsReg::TEXA, gs_texa(0x80, false, 0x80));
}

void GsDevice::draw_textured_triangles(const TexVertex* vertices, uint32_t count)
{
    PS2UR_ASSERT(m_initialized);
    if (vertices == nullptr || count < 3) {
        return;
    }
    const uint32_t triangles = count / 3u;

    m_packet.begin_packed_ad(1);
    m_packet.add_ad(GsReg::PRMODECONT, 1);

    // TME on, and FST=1 so UV carries texel coordinates directly rather than
    // perspective-correct ST/Q. For screen-space geometry that is exactly what
    // we want; ST arrives with the VU1 pipeline at M4.
    const uint64_t prim = gs_prim(GsPrim::Triangle, true, /*textured=*/true, false,
                                  false, false, /*uv_is_st(FST)=*/true, 0, false);
    m_packet.begin_packed(triangles * 3u, 3,
                          gs_reglist(GsReg::RGBAQ, GsReg::UV, GsReg::XYZ2),
                          false, true, prim);
    for (uint32_t i = 0; i < triangles * 3u; ++i) {
        const TexVertex& v = vertices[i];
        m_packet.add_qword(gs_packed_rgbaq(v.r, v.g, v.b, v.a));
        m_packet.add_qword(gs_packed_uv(v.u, v.v));
        m_packet.add_qword(gs_packed_xyz(gs_coord(v.x), gs_coord(v.y), v.z));
    }
}

#if defined(PS2UR_PLATFORM_PS2)
namespace {

// EE hardware registers used for the reverse (GS -> host) transfer. ps2sdk
// provides no helper for this direction, so we drive VIF1 and DMA channel 1
// directly.
volatile uint32_t* const kVif1Stat = reinterpret_cast<volatile uint32_t*>(0x10003C00);
volatile uint32_t* const kD1Chcr = reinterpret_cast<volatile uint32_t*>(0x10009000);
volatile uint32_t* const kD1Madr = reinterpret_cast<volatile uint32_t*>(0x10009010);
volatile uint32_t* const kD1Qwc = reinterpret_cast<volatile uint32_t*>(0x10009020);

constexpr uint32_t kVif1StatFdr = 1u << 23; // VIF1 direction: 1 = GS -> memory
constexpr uint32_t kChcrStr = 1u << 8;      // channel start/busy

} // namespace
#endif

bool GsDevice::read_framebuffer(void* dest, uint32_t x, uint32_t y,
                                uint32_t w, uint32_t h)
{
    if (!m_initialized || dest == nullptr || w == 0 || h == 0) {
        return false;
    }
    if (m_config.colour_format != PixelFormat::PSMCT32) {
        log(LogLevel::Error, "gfx: read_framebuffer only supports PSMCT32");
        return false;
    }
    if ((reinterpret_cast<uintptr_t>(dest) & 15u) != 0) {
        log(LogLevel::Error, "gfx: read_framebuffer destination must be qword aligned");
        return false;
    }

#if defined(PS2UR_PLATFORM_PS2)
    // end_frame() has already advanced the frame index, so the buffer we just
    // finished drawing is the one *behind* the current draw target.
    const uint32_t source_page = display_buffer_page();

    // 1. Describe the transfer over GIF. TRXDIR is written last: it is what
    //    actually arms the transfer, so every other register must be valid
    //    before it lands.
    m_packet.reset();
    m_packet.begin_packed_ad(4);
    m_packet.add_ad(GsReg::BITBLTBUF,
                    gs_bitbltbuf(source_page * kBlocksPerPage,
                                 buffer_width_units(m_config.width),
                                 m_config.colour_format, 0, 0, PixelFormat::PSMCT32));
    m_packet.add_ad(GsReg::TRXPOS, gs_trxpos(x, y, 0, 0, 0));
    m_packet.add_ad(GsReg::TRXREG, gs_trxreg(w, h));
    m_packet.add_ad(GsReg::TRXDIR, 1); // 1 = local -> host
    m_packet.set_last_tag_eop();
    submit_and_wait();
    m_packet.reset();

    // 2. Reverse VIF1 so the FIFO drains from the GS into memory.
    *kVif1Stat = kVif1StatFdr;

    const uint32_t qwords = (w * h * 4u) / 16u;
    FlushCache(0); // ensure no dirty lines overlap the destination

    *kD1Qwc = qwords;
    *kD1Madr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(dest));
    *kD1Chcr = kChcrStr; // STR=1, DIR=0 (to memory), MOD=0 (normal)

    // 3. Wait for the channel to go idle. Bounded so a wedged GS cannot hang
    //    the whole test run -- an unbounded spin here is how a golden-image
    //    job turns into a stuck CI machine.
    uint32_t spins = 0;
    const uint32_t kMaxSpins = 100000000u;
    while ((*kD1Chcr & kChcrStr) != 0) {
        if (++spins > kMaxSpins) {
            *kVif1Stat = 0;
            log(LogLevel::Error, "gfx: read_framebuffer timed out (%u qwords)", u(qwords));
            return false;
        }
    }

    // 4. Restore forward direction, and invalidate so the EE sees what the
    //    DMAC wrote rather than stale cached lines.
    *kVif1Stat = 0;
    FlushCache(0);
    return true;
#else
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    return false; // host build has no GS to read from
#endif
}

uint32_t GsDevice::tile_crc32(const void* pixels, uint32_t w, uint32_t h,
                              uint32_t tile_x, uint32_t tile_y, uint32_t tile_size)
{
    // Standard CRC-32 (reflected, polynomial 0xEDB88320), computed on the fly
    // so there is no 1 KB table sitting in a 32 MB budget.
    if (pixels == nullptr) {
        return 0;
    }
    const uint8_t* base = static_cast<const uint8_t*>(pixels);
    uint32_t crc = 0xFFFFFFFFu;

    const uint32_t x0 = tile_x * tile_size;
    const uint32_t y0 = tile_y * tile_size;
    if (x0 >= w || y0 >= h) {
        return 0;
    }
    const uint32_t x1 = (x0 + tile_size < w) ? x0 + tile_size : w;
    const uint32_t y1 = (y0 + tile_size < h) ? y0 + tile_size : h;

    for (uint32_t py = y0; py < y1; ++py) {
        for (uint32_t px = x0; px < x1; ++px) {
            const uint8_t* p = base + (py * w + px) * 4u;
            // Only RGB participates: the GS leaves alpha in the framebuffer
            // dependent on blend state, which would make goldens brittle.
            for (uint32_t c = 0; c < 3u; ++c) {
                crc ^= p[c];
                for (int bit = 0; bit < 8; ++bit) {
                    const uint32_t mask = static_cast<uint32_t>(-(static_cast<int32_t>(crc) & 1));
                    crc = (crc >> 1) ^ (0xEDB88320u & mask);
                }
            }
        }
    }
    return ~crc;
}

void GsDevice::shutdown()
{
    if (!m_initialized) {
        return;
    }
#if defined(PS2UR_PLATFORM_PS2)
    graph_shutdown();
    dma_channel_shutdown(DMA_CHANNEL_GIF, 0);
#endif
    heap_free(m_packet_memory);
    m_packet_memory = nullptr;
    m_packet.init(nullptr, 0);
    m_vram.reset();
    m_initialized = false;
    log(LogLevel::Debug, "gfx: device shut down after %u frames", u(m_frame_index));
}

} // namespace gfx
} // namespace ps2ur
