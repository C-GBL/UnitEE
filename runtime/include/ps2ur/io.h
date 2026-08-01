// io: CDVD, memory card, p2b container loader (plan sections 3.5, 8: src/io).
// File placement/LBA order on disc is an optimisation lever (section 3.5).
// TODO(spec missing: section 10): p2b container format spec (chunk layout,
// alignment, endianness) -- the loader here stays a stub until that lands.
#pragma once

namespace ps2ur {
namespace io {

bool init();
void shutdown();
bool initialized();

} // namespace io
} // namespace ps2ur
