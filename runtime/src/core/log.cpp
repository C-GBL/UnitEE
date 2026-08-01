#include "ps2ur/log.h"

#include "ps2ur/platform.h"

#include <cstdarg>
#include <cstdio>

namespace ps2ur {

static LogLevel g_min_level = LogLevel::Debug;

void set_log_level(LogLevel level) { g_min_level = level; }
LogLevel get_log_level() { return g_min_level; }

static const char* level_tag(LogLevel level)
{
    switch (level) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

void log_va(LogLevel level, const char* fmt, va_list args)
{
    if (static_cast<int>(level) < static_cast<int>(g_min_level)) {
        return;
    }
    // Fixed buffers: no heap traffic on the log path (32 MB machine, section 3.1).
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, args);
    char line[560];
    snprintf(line, sizeof(line), "[ps2ur:%s] %s", level_tag(level), msg);
    platform::log_sink(static_cast<int>(level), line);
}

void log(LogLevel level, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_va(level, fmt, args);
    va_end(args);
}

} // namespace ps2ur
