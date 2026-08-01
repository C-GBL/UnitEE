/*
 * samples/00-hello-triangle -- plan section 9, M0 task 5.
 *
 * The smallest thing that proves the whole toolchain works end to end:
 * initialise the GS through ps2sdk libgraph/libdraw, clear the screen to a
 * known colour, draw one triangle, and print a token to the EE console so an
 * automated boot test can assert on it (M0 task 6).
 *
 * Deliberately written against raw GIF packets rather than gsKit: ADR-003
 * adopts gsKit only as a reference, and the packet-building style here is the
 * direction M2's gfx::GsPacket takes.
 *
 * This sample is plain C and depends only on ps2sdk -- NOT on ps2ur. It has to
 * keep working even while the runtime is being torn apart.
 */

#include <kernel.h>
#include <tamtypes.h>
#include <stdio.h>

#include <gif_tags.h>
#include <gs_gp.h>
#include <gs_psm.h>

#include <dma.h>
#include <dma_tags.h>

#include <draw.h>
#include <graph.h>
#include <packet.h>

/* Plan section 3.3 baseline: 512x448 NTSC, PSMCT32 colour. */
#define SCREEN_WIDTH  512
#define SCREEN_HEIGHT 448

/* Known clear colour, asserted by the boot test. Deliberately not black or
 * grey so "nothing rendered" cannot be mistaken for "cleared correctly". */
#define CLEAR_R 32
#define CLEAR_G 96
#define CLEAR_B 160

/* The GS screen origin sits at 2048 in 12.4 fixed point (plan section 9, M8:
 * "XYOFFSET origin at 2048"). Screen pixel -> GS coordinate. */
#define GS_X(px) ((((px) + 0) << 4) + (2048 << 4))
#define GS_Y(py) ((((py) + 0) << 4) + (2048 << 4))

static void init_gs(framebuffer_t *frame, zbuffer_t *z)
{
    frame->width = SCREEN_WIDTH;
    frame->height = SCREEN_HEIGHT;
    frame->mask = 0;
    frame->psm = GS_PSM_32;
    frame->address = graph_vram_allocate(frame->width, frame->height,
                                         frame->psm, GRAPH_ALIGN_PAGE);

    /* No depth buffer: this sample draws a single primitive, and leaving Z
     * out keeps the VRAM budget obvious (plan section 15.2). M2 adds it. */
    z->enable = 0;
    z->address = 0;
    z->mask = 0;
    z->zsm = 0;

    graph_initialize(frame->address, frame->width, frame->height,
                     frame->psm, 0, 0);
}

static void init_drawing_environment(packet_t *packet, framebuffer_t *frame,
                                     zbuffer_t *z)
{
    qword_t *q = packet->data;

    q = draw_setup_environment(q, 0, frame, z);
    q = draw_finish(q);

    dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data,
                            q - packet->data, 0, 0);
    draw_wait_finish();
}

static void render(packet_t *packet, framebuffer_t *frame)
{
    qword_t *q;

    dma_wait_fast();

    q = packet->data;
    q = draw_clear(q, 0, 0, 0, frame->width, frame->height,
                   CLEAR_R, CLEAR_G, CLEAR_B);

    /* One Gouraud-shaded triangle, centred. PACKED mode, A+D register writes:
     * PRIM, then per-vertex RGBAQ + XYZ2 (Gouraud needs a colour per vertex),
     * so 1 + 3*2 = 7 registers. */
    PACK_GIFTAG(q, GIF_SET_TAG(7, 1, 0, 0, 0, 1), GIF_REG_AD);
    q++;
    /* prim=3 (triangle), iip=1 (Gouraud), the rest off. */
    PACK_GIFTAG(q, GIF_SET_PRIM(3, 1, 0, 0, 0, 0, 0, 0, 0), GIF_REG_PRIM);
    q++;

    PACK_GIFTAG(q, GIF_SET_RGBAQ(255, 48, 48, 0x80, 0x3F800000), GIF_REG_RGBAQ);
    q++;
    PACK_GIFTAG(q, GIF_SET_XYZ(GS_X(SCREEN_WIDTH / 2), GS_Y(64), 0), GIF_REG_XYZ2);
    q++;

    PACK_GIFTAG(q, GIF_SET_RGBAQ(48, 255, 48, 0x80, 0x3F800000), GIF_REG_RGBAQ);
    q++;
    PACK_GIFTAG(q, GIF_SET_XYZ(GS_X(96), GS_Y(SCREEN_HEIGHT - 64), 0), GIF_REG_XYZ2);
    q++;

    PACK_GIFTAG(q, GIF_SET_RGBAQ(48, 48, 255, 0x80, 0x3F800000), GIF_REG_RGBAQ);
    q++;
    PACK_GIFTAG(q, GIF_SET_XYZ(GS_X(SCREEN_WIDTH - 96), GS_Y(SCREEN_HEIGHT - 64), 0),
                GIF_REG_XYZ2);
    q++;

    q = draw_finish(q);

    dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data,
                            q - packet->data, 0, 0);
    draw_wait_finish();
    graph_wait_vsync();
}

int main(void)
{
    framebuffer_t frame;
    zbuffer_t z;
    packet_t *packet = packet_init(64, PACKET_NORMAL);

    dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);

    init_gs(&frame, &z);
    init_drawing_environment(packet, &frame, &z);
    render(packet, &frame);

    /* The boot test (M0 task 6) greps for this exact token. Keep the string
     * literal and the test in tools/ci/run-emu-test.sh in sync. */
    printf("[00-hello-triangle] GS %dx%d PSMCT32, cleared to (%d,%d,%d), 1 triangle\n",
           SCREEN_WIDTH, SCREEN_HEIGHT, CLEAR_R, CLEAR_G, CLEAR_B);
    printf("PS2UR_TOKEN_HELLO_TRIANGLE_OK\n");

    /* Park rather than return. Returning from main() hands control back to the
     * BIOS, which drops the user on the memory-card browser -- verified on
     * PCSX2 during M0 bring-up. A real title never falls off the end of main. */
    SleepThread();

    return 0;
}
