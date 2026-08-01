// gfx: GS device, VRAM allocator, DMA packet building, texture cache
// (plan section 8: src/gfx; hardware reference sections 3.3-3.4).
// Bring-up order per ADR-003: EE-only PATH3 bootstrap first, then VU1 PATH1.
// TODO(spec missing: section 9): gfx milestone tasks and acceptance criteria.
#pragma once

namespace ps2ur {
namespace gfx {

bool init();
void shutdown();
bool initialized();

} // namespace gfx
} // namespace ps2ur
