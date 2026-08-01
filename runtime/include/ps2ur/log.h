// Logging: printf-style, routed to the platform log sink (plan section 8: src/core).
#pragma once

#include <cstdarg>

#if defined(__GNUC__)
#define PS2UR_PRINTF(fmt_idx, va_idx) __attribute__((format(printf, fmt_idx, va_idx)))
#else
#define PS2UR_PRINTF(fmt_idx, va_idx)
#endif

namespace ps2ur {

enum class LogLevel : int {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
};

// Drop messages below this level. Default: Debug (everything).
void set_log_level(LogLevel level);
LogLevel get_log_level();

void log(LogLevel level, const char* fmt, ...) PS2UR_PRINTF(2, 3);
void log_va(LogLevel level, const char* fmt, va_list args);

} // namespace ps2ur

#define PS2UR_LOG_DEBUG(...) ::ps2ur::log(::ps2ur::LogLevel::Debug, __VA_ARGS__)
#define PS2UR_LOG_INFO(...)  ::ps2ur::log(::ps2ur::LogLevel::Info, __VA_ARGS__)
#define PS2UR_LOG_WARN(...)  ::ps2ur::log(::ps2ur::LogLevel::Warn, __VA_ARGS__)
#define PS2UR_LOG_ERROR(...) ::ps2ur::log(::ps2ur::LogLevel::Error, __VA_ARGS__)
