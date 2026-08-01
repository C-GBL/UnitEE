// audio: SPU2 via IOP (audsrv.irx baseline, custom IRX later) -- 2D + simple 3D
// sources, streamed music, resident SFX (plan sections 3.5, 7.1, 8: src/audio).
// TODO(spec missing: section 9): audio milestone tasks; section 10: VAG/ADPCM asset format.
#pragma once

namespace ps2ur {
namespace audio {

bool init();
void shutdown();
bool initialized();

} // namespace audio
} // namespace ps2ur
