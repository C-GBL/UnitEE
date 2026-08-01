#include "ps2ur/input.h"

#include "ps2ur/log.h"

namespace ps2ur {
namespace input {

static bool g_initialized = false;

bool init()
{
    if (g_initialized) {
        return true;
    }
    // TODO(spec missing: section 9): input milestones. libpad via sio2man.irx +
    // padman.irx (section 3.5); Input.GetKey/GetAxis mapping per section 7.1.
    g_initialized = true;
    log(LogLevel::Debug, "input: init (stub)");
    return true;
}

void shutdown()
{
    if (!g_initialized) {
        return;
    }
    g_initialized = false;
    log(LogLevel::Debug, "input: shutdown (stub)");
}

bool initialized() { return g_initialized; }

} // namespace input
} // namespace ps2ur
