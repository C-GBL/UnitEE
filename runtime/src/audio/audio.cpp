#include "ps2ur/audio.h"

#include "ps2ur/log.h"

namespace ps2ur {
namespace audio {

static bool g_initialized = false;

bool init()
{
    if (g_initialized) {
        return true;
    }
    // TODO(spec missing: section 9): audio milestones. Baseline is audsrv.irx
    // over SIF (section 3.5); SPU2 plays 4-bit ADPCM (VAG), music streams into
    // a ring buffer fed from CDVD.
    g_initialized = true;
    log(LogLevel::Debug, "audio: init (stub)");
    return true;
}

void shutdown()
{
    if (!g_initialized) {
        return;
    }
    g_initialized = false;
    log(LogLevel::Debug, "audio: shutdown (stub)");
}

bool initialized() { return g_initialized; }

} // namespace audio
} // namespace ps2ur
