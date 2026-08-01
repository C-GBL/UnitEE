// Small os:: surfaces for the PS2 EE in one file (plan section 11.2): time,
// console, locale, crypto, allocators, process identity, and the icall
// stragglers. Everything here is either real (time, memory, random) or an
// honest constant for a fixed single-process console target.
#include "il2cpp-config.h"

#if IL2CPP_TARGET_PS2

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "os/Console.h"
#include "os/Cryptography.h"
#include "os/Encoding.h"
#include "os/Environment.h"
#include "os/ErrorCodes.h"
#include "os/FileSystemWatcher.h"
#include "os/Image.h"
#include "os/LastError.h"
#include "os/LibraryLoader.h"
#include "os/Locale.h"
#include "os/MarshalAlloc.h"
#include "os/Memory.h"
#include "os/NativeMethods.h"
#include "os/Process.h"
#include "os/StackTrace.h"
#include "os/Time.h"
#include "os/TimeZone.h"
#include "gc/GarbageCollector.h"
#include "icalls/mscorlib/System.Runtime/RuntimeImports.h"
#include "utils/Expected.h"

// ---- clock ---------------------------------------------------------------
//
// COP0 Count increments at half the EE core clock: 147.456 MHz (same basis as
// runtime/src/platform/ps2/platform_ps2.cpp). The register is 32-bit and
// wraps every ~29 s, so it is widened here; single-threaded, so two statics
// are safe.
namespace
{
    uint64_t ReadClock64()
    {
        uint32_t now;
        asm volatile ("mfc0 %0, $9" : "=r" (now));
        static uint32_t s_Last = 0;
        static uint64_t s_High = 0;
        if (now < s_Last)
            s_High += 0x100000000ULL;
        s_Last = now;
        return s_High | now;
    }

    const int64_t kClockHz = 147456000;

    int64_t ClockTo100Ns(uint64_t ticks)
    {
        // 1e7 / 147456000 == 625 / 9216, exactly.
        return static_cast<int64_t>(ticks * 625ULL / 9216ULL);
    }

    // No RTC without the cdvd RPC (deliberately not loaded for M6): wall
    // time is a fixed epoch plus uptime. 100 ns intervals 1601-01-01 to
    // 2026-01-01 (Unix 1767225600 plus the 1601->1970 offset).
    const int64_t kFileTimeAtBoot = 116444736000000000LL + 1767225600LL * 10000000LL;
}

namespace il2cpp
{
namespace os
{
    // ---- Time ----------------------------------------------------------

    uint32_t Time::GetTicksMillisecondsMonotonic()
    {
        return static_cast<uint32_t>(ReadClock64() / (kClockHz / 1000));
    }

    int64_t Time::GetTicks100NanosecondsMonotonic()
    {
        return ClockTo100Ns(ReadClock64());
    }

    int64_t Time::GetSystemTimeAsFileTime()
    {
        return kFileTimeAtBoot + ClockTo100Ns(ReadClock64());
    }

    int64_t Time::GetTicks100NanosecondsDateTime()
    {
        // FILETIME base (1601) to DateTime base (0001), same constant the
        // Win32 implementation uses.
        const int64_t kFileTimeAdjust = 504911232000000000LL;
        return kFileTimeAdjust + GetSystemTimeAsFileTime();
    }

    // ---- TimeZone ------------------------------------------------------

    bool TimeZone::GetTimeZoneData(int32_t year, int64_t data[4], std::string names[2], bool* daylight_inverted)
    {
        data[0] = 0; // DST start
        data[1] = 0; // DST end
        data[2] = 0; // UTC offset, ticks
        data[3] = 0; // DST delta
        names[0] = "UTC";
        names[1] = "UTC";
        *daylight_inverted = false;
        return true;
    }

    // ---- Console -------------------------------------------------------

    namespace Console
    {
        bool SetBreak(bool wantBreak)
        {
            return true;
        }

        bool SetEcho(bool wantEcho)
        {
            return true;
        }

        bool TtySetup(const std::string& keypadXmit, const std::string& teardown, uint8_t* control_characters, int32_t** size)
        {
            return false; // the EE TTY is write-only
        }

        int32_t InternalKeyAvailable(int32_t ms_timeout)
        {
            return 0;
        }
    }

    // ---- Locale / Encoding ---------------------------------------------

    void Locale::Initialize()
    {
    }

    void Locale::UnInitialize()
    {
    }

    std::string Locale::GetLocale()
    {
        return "en-US";
    }

    namespace Encoding
    {
        std::string GetCharSet()
        {
            return "UTF-8";
        }
    }

    // ---- Image / StackTrace / LastError --------------------------------

    namespace Image
    {
        void Initialize()
        {
        }

        void* GetImageBase()
        {
            // Standard EE ELF load base; only compared for identity.
            return reinterpret_cast<void*>(0x00100000);
        }
    }

    void StackTrace::WalkStackNative(WalkStackCallback callback, void* context, WalkOrder walkOrder)
    {
        // IL2CPP_ENABLE_STACKTRACES=0: never called with a real consumer.
    }

    uint32_t LastError::GetLastError()
    {
        return 0;
    }

    // ---- Cryptography --------------------------------------------------
    //
    // No hardware entropy source. xorshift64* seeded from the cycle counter
    // is documented as NOT cryptographically secure (plan 11.2: GetRandom
    // quality is a known reduction on this target); it exists so that
    // Guid.NewGuid and Random-backed BCL paths function.

    static uint64_t s_RngState;

    void* Cryptography::GetCryptographyProvider()
    {
        return reinterpret_cast<void*>(1);
    }

    bool Cryptography::OpenCryptographyProvider()
    {
        s_RngState = ReadClock64() | 1;
        return true;
    }

    void Cryptography::ReleaseCryptographyProvider(void* provider)
    {
    }

    bool Cryptography::FillBufferWithRandomBytes(void* provider, intptr_t length, unsigned char* data)
    {
        if (s_RngState == 0)
            s_RngState = ReadClock64() | 1;
        for (intptr_t i = 0; i < length; ++i)
        {
            s_RngState ^= s_RngState >> 12;
            s_RngState ^= s_RngState << 25;
            s_RngState ^= s_RngState >> 27;
            data[i] = static_cast<unsigned char>((s_RngState * 0x2545F4914F6CDD1DULL) >> 56);
        }
        return true;
    }

    // ---- Memory / MarshalAlloc -----------------------------------------
    //
    // Aligned blocks carry {raw pointer, usable size} directly below the
    // aligned address so ReAlloc knows how much to preserve; newlib's
    // memalign has no usable-size query.

    namespace Memory
    {
        void* AlignedAlloc(size_t size, size_t alignment)
        {
            if (alignment < sizeof(void*))
                alignment = sizeof(void*);
            void* raw = malloc(size + alignment + 2 * sizeof(size_t));
            if (raw == NULL)
                return NULL;
            uintptr_t user = (reinterpret_cast<uintptr_t>(raw) + 2 * sizeof(size_t) + alignment - 1) & ~(alignment - 1);
            reinterpret_cast<size_t*>(user)[-1] = size;
            reinterpret_cast<size_t*>(user)[-2] = reinterpret_cast<size_t>(raw);
            return reinterpret_cast<void*>(user);
        }

        void AlignedFree(void* memory)
        {
            if (memory != NULL)
                free(reinterpret_cast<void*>(reinterpret_cast<size_t*>(memory)[-2]));
        }

        void* AlignedReAlloc(void* memory, size_t newSize, size_t alignment)
        {
            if (memory == NULL)
                return AlignedAlloc(newSize, alignment);
            void* fresh = AlignedAlloc(newSize, alignment);
            if (fresh != NULL)
            {
                const size_t oldSize = reinterpret_cast<size_t*>(memory)[-1];
                memcpy(fresh, memory, oldSize < newSize ? oldSize : newSize);
            }
            AlignedFree(memory);
            return fresh;
        }
    }

    void* MarshalAlloc::Allocate(size_t size)
    {
        return malloc(size);
    }

    void* MarshalAlloc::ReAlloc(void* ptr, size_t size)
    {
        return realloc(ptr, size);
    }

    void MarshalAlloc::Free(void* ptr)
    {
        free(ptr);
    }

    void* MarshalAlloc::AllocateHGlobal(size_t size)
    {
        return malloc(size);
    }

    void* MarshalAlloc::ReAllocHGlobal(void* ptr, size_t size)
    {
        return realloc(ptr, size);
    }

    void MarshalAlloc::FreeHGlobal(void* ptr)
    {
        free(ptr);
    }

    // ---- Environment / FileSystemWatcher -------------------------------

    utils::Expected<std::string> Environment::GetWindowsFolderPath(int32_t folder)
    {
        return std::string("host:");
    }

    namespace FileSystemWatcher
    {
        int IsSupported()
        {
            return 0;
        }
    }

    // ---- Process / NativeMethods ---------------------------------------
    //
    // Exactly one process exists and it is us; every handle is the same
    // sentinel.

    static ProcessHandle* SelfHandle()
    {
        return reinterpret_cast<ProcessHandle*>(1);
    }

    int Process::GetCurrentProcessId()
    {
        return 1;
    }

    utils::Expected<ProcessHandle*> Process::GetProcess(int processId)
    {
        return SelfHandle();
    }

    utils::Expected<std::string> Process::GetProcessName(ProcessHandle* handle)
    {
        return std::string("m6-hello");
    }

    intptr_t Process::GetMainWindowHandle(int32_t pid)
    {
        return 0;
    }

    bool NativeMethods::CloseProcess(ProcessHandle* handle)
    {
        return true;
    }

    utils::Expected<bool> NativeMethods::GetExitCodeProcess(ProcessHandle* handle, int32_t* exitCode)
    {
        *exitCode = 0;
        return true;
    }

    utils::Expected<ProcessHandle*> NativeMethods::GetCurrentProcess()
    {
        return SelfHandle();
    }

    int32_t NativeMethods::GetCurrentProcessId()
    {
        return 1;
    }

    // ---- LibraryLoader -------------------------------------------------
    //
    // Static link, no loader: every probe misses. P/Invoke targets would be
    // wired through the hardcoded table when a later milestone needs one.

    const HardcodedPInvokeDependencyLibrary* LibraryLoader::HardcodedPInvokeDependencies = NULL;
    const size_t LibraryLoader::HardcodedPInvokeDependenciesCount = 0;

    Baselib_DynamicLibrary_Handle LibraryLoader::ProbeForLibrary(const Il2CppNativeChar* libraryName, const size_t libraryNameLength, std::string& detailedError)
    {
        detailedError = "dynamic libraries do not exist on the PS2 target";
        return Baselib_DynamicLibrary_Handle_Invalid;
    }

    Baselib_DynamicLibrary_Handle LibraryLoader::OpenProgramHandle(Baselib_ErrorState& errorState, bool& needsClosing)
    {
        needsClosing = false;
        errorState.code = Baselib_ErrorCode_NotSupported;
        return Baselib_DynamicLibrary_Handle_Invalid;
    }
}

// ---- gc odds and ends ----------------------------------------------------
//
// Null-GC bring-up only (ADR-004): nothing is ever collected, so roots need
// no registration and ephemerons behave as strong references. BoehmGC.cpp
// provides the real implementations when IL2CPP_GC_BOEHM is on.

#if !IL2CPP_GC_BOEHM
namespace gc
{
    void GarbageCollector::RegisterRoot(char* start, size_t size)
    {
    }

    void GarbageCollector::UnregisterRoot(char* start)
    {
    }

    bool GarbageCollector::EphemeronArrayAdd(Il2CppObject* obj)
    {
        return true;
    }
}
#endif // !IL2CPP_GC_BOEHM

// ---- icalls: RuntimeImports.ecvt_s ---------------------------------------
//
// Windows-only upstream; the EE build supplies it via printf %e and digit
// extraction. Used by Number.ToString for doubles (software floats on the
// EE -- formatting is not a hot path).

namespace icalls
{
namespace mscorlib
{
namespace System
{
namespace Runtime
{
    void RuntimeImports::ecvt_s(char* buffer, int32_t sizeInBytes, double value, int32_t count, int32_t* dec, int32_t* sign)
    {
        *sign = value < 0.0 ? 1 : 0;
        if (value < 0.0)
            value = -value;

        if (count < 1)
            count = 1;
        if (count > sizeInBytes - 1)
            count = sizeInBytes - 1;
        if (count > 17)
            count = 17;

        if (!(value > 0.0) || value != value) // zero, NaN (callers pre-filter non-finite)
        {
            for (int32_t i = 0; i < count; ++i)
                buffer[i] = '0';
            buffer[count] = '\0';
            *dec = 0;
            return;
        }

        char formatted[48];
        snprintf(formatted, sizeof(formatted), "%.*e", static_cast<int>(count - 1), value);

        int32_t written = 0;
        const char* p = formatted;
        for (; *p != '\0' && *p != 'e' && *p != 'E'; ++p)
        {
            if (*p >= '0' && *p <= '9' && written < count)
                buffer[written++] = *p;
        }
        while (written < count)
            buffer[written++] = '0';
        buffer[written] = '\0';

        int32_t exponent = 0;
        if (*p == 'e' || *p == 'E')
            exponent = static_cast<int32_t>(strtol(p + 1, NULL, 10));
        *dec = exponent + 1;
    }
}
}
}
}
}

#endif // IL2CPP_TARGET_PS2
