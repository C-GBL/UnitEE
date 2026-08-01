// libatomic out-of-line shims for the EE (plan section 11.2).
//
// GCC lowers some __atomic builtins on mips r5900 to library calls instead
// of ll/sc inline (notably 8-byte operations and a few 4-byte forms in
// libil2cpp's os/Atomic.h). There is no libatomic in the ps2sdk toolchain;
// with IL2CPP_SUPPORT_THREADS=0 and no signal-driven reentrancy, plain
// loads and stores ARE the correct implementation, not an approximation.
// Signatures follow the documented libatomic library ABI.
#include "il2cpp-config.h"

#if IL2CPP_TARGET_PS2

#include <stdint.h>
#include <string.h>

extern "C"
{
    // Parameter types spelled to match gcc's builtin declarations exactly
    // (unsigned int / unsigned long long); uint32_t is `unsigned long` on
    // the EE and ambiguates them.
    unsigned int __atomic_fetch_add_4(volatile void* mem, unsigned int value, int model)
    {
        volatile unsigned int* p = static_cast<volatile unsigned int*>(mem);
        const unsigned int old = *p;
        *p = old + value;
        return old;
    }

    unsigned long long __atomic_fetch_add_8(volatile void* mem, unsigned long long value, int model)
    {
        volatile unsigned long long* p = static_cast<volatile unsigned long long*>(mem);
        const unsigned long long old = *p;
        *p = old + value;
        return old;
    }

    unsigned int __atomic_exchange_4(volatile void* mem, unsigned int value, int model)
    {
        volatile unsigned int* p = static_cast<volatile unsigned int*>(mem);
        const unsigned int old = *p;
        *p = value;
        return old;
    }

    bool __atomic_compare_exchange_4(volatile void* mem, void* expected, unsigned int desired, bool weak, int success, int failure)
    {
        volatile unsigned int* p = static_cast<volatile unsigned int*>(mem);
        unsigned int* e = static_cast<unsigned int*>(expected);
        if (*p == *e)
        {
            *p = desired;
            return true;
        }
        *e = *p;
        return false;
    }

    bool __atomic_compare_exchange_8(volatile void* mem, void* expected, unsigned long long desired, bool weak, int success, int failure)
    {
        volatile unsigned long long* p = static_cast<volatile unsigned long long*>(mem);
        unsigned long long* e = static_cast<unsigned long long*>(expected);
        if (*p == *e)
        {
            *p = desired;
            return true;
        }
        *e = *p;
        return false;
    }
}

#endif // IL2CPP_TARGET_PS2
