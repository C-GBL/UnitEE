// GS device: video mode, double buffering, drawing environment, frame loop
// (plan section 9, M2 tasks 1 and 4).
//
// Split of responsibilities: ps2sdk's libgraph owns the CRTC side (SMODE /
// PMODE / DISPLAY and the region-dependent timing tables), because that is
// tedious, well-solved, and changes only at init. Everything the renderer
// touches per frame -- FRAME_1, ZBUF_1, XYOFFSET_1, SCISSOR_1, TEST_1, and
// every primitive -- is built here as GIF packets through GsPacket, because
// that is the path M4 replaces with VU1/PATH1 and it must be ours.
//
// ADR-003: this is the EE-side PATH3 bootstrap. It is deliberately the slow
// path; its job is to unblock M3/M5 and to be the golden-image reference that
// the VU1 renderer is later diffed against.
#pragma once

#include "ps2ur/gs_format.h"
#include "ps2ur/gs_packet.h"
#include "ps2ur/gs_vram.h"

#include <cstdint>

namespace ps2ur {
namespace gfx {

enum class VideoStandard : uint8_t { NTSC, PAL };

struct VideoConfig {
    uint32_t width = 512;
    uint32_t height = 448;
    VideoStandard standard = VideoStandard::NTSC;
    bool interlaced = true;
    PixelFormat colour_format = PixelFormat::PSMCT32;
    PixelFormat depth_format = PixelFormat::PSMZ24;
    bool depth_enabled = true;

    // Qwords reserved for the per-frame packet. 512x448 clear + a few hundred
    // primitives fits comfortably; overflow is reported, never silent.
    uint32_t packet_qwords = 8192;
};

class GsDevice {
public:
    bool init(const VideoConfig& config);
    void shutdown();
    bool initialized() const { return m_initialized; }

    const VideoConfig& config() const { return m_config; }
    VramAllocator& vram() { return m_vram; }

    // Frame loop. Draw calls go between begin_frame() and end_frame(); the
    // packet is submitted and the buffers flipped by end_frame().
    void begin_frame();
    void end_frame();

    // Full-screen clear, implemented as a sprite primitive with the depth test
    // forced to ALWAYS and Z-write on, so it also resets the Z buffer
    // (plan M2 task 4).
    void clear(uint8_t r, uint8_t g, uint8_t b, uint32_t depth = 0);

    // Immediate-mode triangles: vertices are already in screen space and
    // 12.4 fixed point is applied here. EE-side transform, PATH3 upload --
    // slow and temporary (ADR-003), but it unblocks everything downstream.
    struct Vertex {
        int32_t x; // screen pixels
        int32_t y;
        uint32_t z;
        uint8_t r, g, b, a; // a: 0x80 is fully opaque on this hardware
    };
    void draw_triangles_immediate(const Vertex* vertices, uint32_t count);

    // The packet being built this frame, for code that wants to append raw
    // GIF data (texture uploads, the debug overlay).
    GsPacket& packet() { return m_packet; }

    // Uploads pixel data into VRAM via GIF IMAGE mode (plan section 9, M2
    // task 6). 'dest' must have been reserved from vram(); 'data' is w*h
    // pixels already in the target format, and for indexed formats already
    // swizzled by the exporter (plan section 3.3 -- swizzling at runtime wastes
    // EE cycles).
    //
    // Uploads are appended to the current frame packet, so call this between
    // begin_frame() and end_frame(). Plan section 3.4 warns that a PATH3
    // upload while VU1 is drawing over PATH1 will stall; once M4 lands, keep
    // uploads at frame boundaries.
    bool upload_texture(const void* data, const VramAlloc& dest, uint32_t w,
                        uint32_t h, PixelFormat fmt);

    // Binds a texture for subsequent draws. 'fmt' and the dimensions must
    // match what was uploaded. Dimensions are powers of two.
    void set_texture(const VramAlloc& tex, uint32_t w, uint32_t h, PixelFormat fmt);

    // Textured triangles. UVs are in 12.4 fixed point (see gs_packed_uv), so
    // texel (1,1) is u=16, v=16.
    struct TexVertex {
        int32_t x;
        int32_t y;
        uint32_t z;
        uint32_t u;
        uint32_t v;
        uint8_t r, g, b, a; // modulated with the texel
    };
    void draw_textured_triangles(const TexVertex* vertices, uint32_t count);

    // Reads a rectangle of the buffer that was most recently drawn into, back
    // out of VRAM into main memory (plan section 14.3, M2 acceptance).
    //
    // This is the GS local->host path: the transfer is set up over GIF, then
    // VIF1 is reversed and the pixels are DMA'd back. It is slow and stalls
    // the whole pipeline -- it exists for golden-image tests and debugging,
    // never for per-frame work.
    //
    // 'dest' must be 16-byte aligned with room for w*h*4 bytes (PSMCT32 only
    // for now). Must be called after end_frame(). Returns false if the request
    // is malformed or the transfer times out.
    bool read_framebuffer(void* dest, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

    // CRC32 of a 32x32 tile grid over a PSMCT32 image, per plan section 14.3.
    // Tile granularity makes a golden-image failure localisable ("the
    // bottom-right changed") instead of a single opaque mismatch.
    static uint32_t tile_crc32(const void* pixels, uint32_t w, uint32_t h,
                               uint32_t tile_x, uint32_t tile_y,
                               uint32_t tile_size = 32);

    uint32_t frame_index() const { return m_frame_index; }

    // Logs each step of packet submission. Bring-up aid: when the GS path
    // wedges, this is what tells you whether it stalled before the DMA, in the
    // DMA, or after. Costs a printf per step, so enable it for a frame or two,
    // never for a whole run.
    void set_trace(bool enabled) { m_trace = enabled; }

private:
    void build_drawing_environment();
    void submit_and_wait();
    uint32_t draw_buffer_page() const;
    uint32_t display_buffer_page() const;

    VideoConfig m_config;
    VramAllocator m_vram;
    VramAlloc m_colour[2];
    VramAlloc m_depth;

    GsPacket m_packet;
    Qword* m_packet_memory = nullptr;

    uint32_t m_frame_index = 0;
    bool m_initialized = false;
    bool m_trace = false;
};

} // namespace gfx
} // namespace ps2ur
