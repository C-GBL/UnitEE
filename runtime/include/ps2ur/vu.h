// vu: VU1 microprogram manager + .vsm sources (plan section 8: src/vu;
// hardware reference section 3.2). Microprograms are assembled with dvp-as
// (section 4.1) once the ps2dev toolchain lands and are embedded as blobs.
// TODO(spec missing: section 9): microprogram set and upload/double-buffer plan.
#pragma once

namespace ps2ur {
namespace vu {

bool init();
void shutdown();
bool initialized();

} // namespace vu
} // namespace ps2ur
