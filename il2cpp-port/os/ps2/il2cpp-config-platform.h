// IL2CPP platform configuration for the PlayStation 2 EE (plan section 11.2).
//
// Pulled in by os/c-api/il2cpp-config-platforms.h through the sanctioned
// IL2CPP_USE_PLATFORM_CONFIG hook ("defined handled externally"), which the
// EE build defines on the command line. Everything not set here falls through
// the header's own #ifndef defaults -- every IL2CPP_TARGET_* becomes 0, the
// architecture chain resolves to none-of-the-above, and __LP64__ being absent
// gives 32-bit pointers. That default cascade is exactly right for a bare
// 32-bit MIPS target, which is why this file is so short.
#pragma once

// New target (plan 11.2 table). Not consulted by upstream code -- it gates
// OUR os/ps2 sources.
#define IL2CPP_TARGET_PS2 1

// Single-threaded first (plan 11.3: "do not do v2 during M6"). This disables
// the thread-implementation requirement entirely; the managed scheduler is
// cooperative.
#define IL2CPP_SUPPORT_THREADS 0

// bdwgc (plan M6 task 4; PS2 stanza in the gcconfig.h patch, hooks in
// os/ps2/GcSupport.cpp). Bring-up happened on the null GC first (ADR-004);
// set to 0 to fall back to it when isolating GC-vs-everything-else bugs.
#define IL2CPP_GC_BOEHM 1

// No stack traces on target (plan 11.2: size, and there is no unwinder
// integration).
#define IL2CPP_ENABLE_STACKTRACES 0
