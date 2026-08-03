// Scene renderer (plan section 9, M8): the frame path that makes the
// renderer an engine instead of a demo. Per frame:
//
//   cull      per-mesh bounding spheres against the camera frustum, plus
//             layer-mask and active-chain filtering
//   queue     surviving draws into RenderQueue with packed sort keys
//   sort      radix; opaque front-to-back, transparent back-to-front
//   emit      grouped by (pass, kind, texture): one material-state flush and
//             texture bind per group boundary, then constants + batches per
//             entity into the VU1 chain, kicked per group
//
// Cameras: perspective or orthographic, viewport rect (screen mapping +
// scissor), clear flags/colour, layer culling mask (M8 task 2). Fog colour
// is set when the camera asks for it; per-vertex F needs the fog VU layout
// and is gated per batch by PRIM.FGE (task 7).
#pragma once

#include "ps2ur/dma_chain.h"
#include "ps2ur/gs_device.h"
#include "ps2ur/p2b_scene.h"
#include "ps2ur/render_queue.h"

#include <cstdint>

namespace ps2ur {
namespace scene {

struct RendererPrograms {
    uint32_t unlit_addr = 0;
    uint32_t tex_addr = 300;
    uint32_t lit_addr = 700;
    uint32_t lit_fog_addr = 1000;
    uint32_t skin_addr = 1300;
    // Textured skinning (M12.5): 6-qword vertices, ST+RGBAQ+XYZ2 out. Only
    // hosts whose content is textured need to upload it; the format flag in
    // the SKMS header decides which program a mesh runs on.
    uint32_t skin_tex_addr = 1500;
};

struct RenderStats {
    uint32_t considered = 0;    // alive, visible, has a mesh
    uint32_t culled = 0;        // rejected by frustum or layer mask
    uint32_t drawn = 0;         // draws that reached the chain
    uint32_t kicks = 0;         // chain kicks (group boundaries)
    uint32_t skinned_drawn = 0; // skinned characters drawn (M9)
    uint32_t skin_batches = 0;  // skinned batches submitted
};

class SceneRenderer {
public:
    // Rebinding textures is device- and sample-specific (VRAM allocations
    // live with the caller), so it arrives as a callback invoked between
    // kicks, never during PATH1 traffic.
    typedef void (*BindTextureFn)(void* user, uint32_t texture_index);

    // Invoked inside the frame packet after the clear: the 2D/UI pass
    // (overlay text, HUD sprites) rides the same PATH3 packet as the clear.
    typedef void (*OverlayFn)(void* user);

    // Renders one frame: clear per camera flags, cull, sort, emit. The
    // world's matrices must be current (call update_world_matrices first).
    // Returns false on chain overflow/kick failure.
    bool render(gfx::GsDevice& device, gfx::DmaChain& chain, World& world,
                const RendererPrograms& programs, BindTextureFn bind_texture,
                void* bind_user, RenderStats* stats,
                OverlayFn overlay = nullptr, void* overlay_user = nullptr);

private:
    gfx::RenderQueue m_queue;
};

} // namespace scene
} // namespace ps2ur
