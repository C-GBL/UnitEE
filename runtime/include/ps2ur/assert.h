// PS2UR_ASSERT: fatal assertion macro. Compiled out when NDEBUG is defined.
#pragma once

namespace ps2ur {
namespace detail {

// Logs the failed expression and aborts. Never returns.
[[noreturn]] void assert_fail(const char* expr, const char* file, int line);

} // namespace detail
} // namespace ps2ur

#if defined(NDEBUG)
#define PS2UR_ASSERT(cond) ((void)0)
#else
#define PS2UR_ASSERT(cond) \
    ((cond) ? (void)0 : ::ps2ur::detail::assert_fail(#cond, __FILE__, __LINE__))
#endif
