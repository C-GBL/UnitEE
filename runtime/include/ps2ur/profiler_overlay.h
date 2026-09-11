// The profiler's live overlay (plan section 9, M13 task 1: "output as a live
// overlay and as a CSV").
//
// Three pages, cycled with a button, because three different questions get
// asked of a frame and one screen answering all of them answers none well:
//
//   Frame   is the game hitting 30 fps, and when it misses, is the EE busy
//           or waiting for the pipeline? (Plan section 15.3: if the EE is
//           not idle at the end of the frame, profile the EE first.)
//   Zones   which piece of the frame is the expensive one.
//   Memory  the memory map screen from task 2, against the section 15.1
//           budgets.
//
// Drawing costs one overlay pass and no allocation. It is deliberately drawn
// LAST, after the scene, so it never perturbs what it is measuring beyond a
// fixed and visible amount.
#pragma once

#include <cstdint>

namespace ps2ur {

namespace gfx {
class GsDevice;
class DebugOverlay;
} // namespace gfx

namespace prof {

enum class Page : uint8_t {
    Off = 0,
    Frame,
    Zones,
    Memory,
    Vram, // every VRAM allocation by name, and the occupancy map
    Count,
};

void set_page(Page p);
Page page();
void next_page(); // wraps through Off, which is how you turn it off

// Draws the current page. A no-op when the page is Off, so a shipping build
// can leave the call in place.
void draw_overlay(gfx::GsDevice& device, gfx::DebugOverlay& overlay,
                  int32_t screen_w, int32_t screen_h);

} // namespace prof
} // namespace ps2ur
