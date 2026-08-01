#pragma once
// Baselib platform environment for the PlayStation 2 EE (our own file,
// overlaid; modelled on the WebGL no-pthreads variant, the closest shipped
// single-threaded platform). IL2CPP_SUPPORT_THREADS is 0 for this port, so
// the synchronisation primitives only need to exist, not to block.

// Matches the no-thread-implementation semaphore: one 32-bit counter.
enum { Baselib_SystemSemaphore_PlatformSize = 4 };

#ifndef EXPORTED_SYMBOL
    #define EXPORTED_SYMBOL __attribute__((visibility("default")))
#endif
#ifndef IMPORTED_SYMBOL
    #define IMPORTED_SYMBOL
#endif

// No debugger hook on the EE; a break instruction would take down the run.
#define BASELIB_DEBUG_TRAP() ((void)0)

#ifndef PLATFORM_PROPERTY_CACHE_LINE_SIZE
    // EE data cache line size.
    #define PLATFORM_PROPERTY_CACHE_LINE_SIZE 64
#endif

#ifndef PLATFORM_HAS_NATIVE_FUTEX
    #define PLATFORM_HAS_NATIVE_FUTEX 0
#endif
#ifndef PLATFORM_HAS_NATIVE_LLSC
    // The R5900 does have LL/SC, but single-threaded baselib never needs it.
    #define PLATFORM_HAS_NATIVE_LLSC 0
#endif
#ifndef PLATFORM_HAS_POSIX_SOCKET_IPV6_SUPPORT
    #define PLATFORM_HAS_POSIX_SOCKET_IPV6_SUPPORT 0
#endif
#ifndef PLATFORM_PROPERTY_MEMORY_MALLOC_MIN_ALIGNMENT
    // newlib malloc on the EE aligns to 8; qword data uses memalign anyway.
    #define PLATFORM_PROPERTY_MEMORY_MALLOC_MIN_ALIGNMENT 8
#endif
