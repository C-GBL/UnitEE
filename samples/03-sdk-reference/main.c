/*
 * samples/03-sdk-reference -- a control, not a deliverable.
 *
 * Clears the screen to deep blue using ONLY ps2sdk (libgraph + libdraw), at
 * exactly the video configuration ps2ur uses: 512x448 NTSC, PSMCT32. It links
 * none of our code.
 *
 * Purpose: 02-gs-clear renders black, and draw_wait_finish() hangs when used
 * with our packets, which together suggest our GIF data never reaches the GS.
 * This control tells us which half is at fault:
 *
 *   blue screen  -> ps2sdk at 512x448 is fine; the bug is in ps2ur's packet
 *                   building or DMA submission (GsPacket / GsDevice).
 *   black screen -> the video mode setup itself is wrong at this resolution,
 *                   and ps2ur is innocent.
 *
 * It deliberately follows $PS2SDK/samples/graph/graph.c step for step,
 * including dma_channel_fast_waits + dma_wait_fast + draw_wait_finish, since
 * that combination is known to work.
 */

#include <kernel.h>
#include <stdio.h>
#include <tamtypes.h>

#include <dma.h>
#include <dma_tags.h>
#include <draw.h>
#include <gif_tags.h>
#include <graph.h>
#include <gs_gp.h>
#include <gs_psm.h>
#include <packet.h>

#define SCREEN_WIDTH 512
#define SCREEN_HEIGHT 448

int main(void)
{
    framebuffer_t frame;
    zbuffer_t z;
    packet_t *packet;
    qword_t *q;
    int i;

    packet = packet_init(64, PACKET_NORMAL);

    dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);

    /* Same buffer layout ps2ur asks for: 512x448 PSMCT32 plus a Z buffer. */
    frame.width = SCREEN_WIDTH;
    frame.height = SCREEN_HEIGHT;
    frame.mask = 0;
    frame.psm = GS_PSM_32;
    frame.address = graph_vram_allocate(frame.width, frame.height, frame.psm,
                                        GRAPH_ALIGN_PAGE);

    z.enable = 1;
    z.method = ZTEST_METHOD_GREATER_EQUAL;
    z.address = graph_vram_allocate(frame.width, frame.height, GS_PSMZ_24,
                                    GRAPH_ALIGN_PAGE);
    z.mask = 0;
    z.zsm = GS_PSMZ_24;

    printf("[03-sdk-reference] frame page %d, z page %d\n", frame.address, z.address);

    graph_initialize(frame.address, frame.width, frame.height, frame.psm, 0, 0);

    /* Drawing environment, exactly as the SDK sample does it. */
    q = packet->data;
    q = draw_setup_environment(q, 0, &frame, &z);
    q = draw_finish(q);
    dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
    draw_wait_finish();

    for (i = 0; i < 180; i++) {
        dma_wait_fast();

        q = packet->data;
        q = draw_clear(q, 0, 0, 0, frame.width, frame.height, 20, 40, 160);
        q = draw_finish(q);

        dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
        draw_wait_finish();
        graph_wait_vsync();
    }

    printf("[03-sdk-reference] 180 frames done, screen should be DEEP BLUE\n");
    printf("PS2UR_TOKEN_SDK_REFERENCE_OK\n");

    SleepThread();
    return 0;
}
