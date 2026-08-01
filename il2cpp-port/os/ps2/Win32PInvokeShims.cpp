// Static implementations of the Win32 API surface the unityaot-win32 BCL
// P/Invokes (plan section 11.5).
//
// The BCL profile shipped with the Unity Editor on Windows is the win32
// flavor: System.Console, TimeZoneInfo, registry and crypto paths carry
// DllImport("kernel32.dll") etc. compiled in. IL2CPP's generated code wraps
// every such call site in FORCE_PINVOKE_<lib>_INTERNAL -- the console-
// platform mechanism that turns them into direct extern "C" calls resolved
// at link time. The EE build defines those macros and this file is the
// receiving end.
//
// Behavior contract: honest failure. Console probes report "not a console"
// (output still flows through MonoIO to the EE TTY), the registry reports
// missing keys, time zone queries report UTC, and BCryptGenRandom is the
// same non-cryptographic generator os::Cryptography uses. Nothing here
// pretends a capability exists.
#include "il2cpp-config.h"

#if IL2CPP_TARGET_PS2

#include <stdint.h>
#include <string.h>

#include "os/Cryptography.h"

// UTF-16 code unit, matching Il2CppChar in the generated declarations.
typedef unsigned short PS2_WCHAR;

namespace
{
    // ERROR_* / STATUS_* values the BCL compares against.
    const int32_t kErrorFileNotFound = 2;
    const int32_t kErrorNoMoreItems = 259;
    const uint32_t kTimeZoneIdUnknown = 0;
    const int32_t kCpUtf8 = 65001;

    size_t WideLength(const PS2_WCHAR* s)
    {
        size_t n = 0;
        while (s[n] != 0)
            ++n;
        return n;
    }
}

extern "C"
{
    // ---- console (kernel32) --------------------------------------------

    int32_t GetConsoleCP()
    {
        return kCpUtf8;
    }

    int32_t GetConsoleOutputCP()
    {
        return kCpUtf8;
    }

    intptr_t GetStdHandle(int32_t nStdHandle)
    {
        // Opaque nonzero token; only ever handed back to the stubs below.
        return static_cast<intptr_t>(nStdHandle);
    }

    int32_t GetConsoleScreenBufferInfo(intptr_t handle, void* info)
    {
        return 0; // FALSE: not a screen console -> BCL uses plain streams
    }

    int32_t ReadConsoleInput(intptr_t handle, void* record, int32_t length, int32_t* numberRead)
    {
        if (numberRead != NULL)
            *numberRead = 0;
        return 0;
    }

    // ---- loader / resources (kernel32, user32) -------------------------

    void* LoadLibraryExW(PS2_WCHAR* name, intptr_t file, int32_t flags)
    {
        return NULL;
    }

    int32_t FreeLibrary(intptr_t module)
    {
        return 0;
    }

    int32_t LoadStringW(void* instance, int32_t id, PS2_WCHAR* buffer, int32_t bufferMax)
    {
        return 0;
    }

    int32_t GetFileMUIPath(uint32_t flags, PS2_WCHAR* filePath, PS2_WCHAR* language, int32_t* languageLength,
        PS2_WCHAR* fileMUIPath, int32_t* fileMUIPathLength, int64_t* enumerator)
    {
        return 0;
    }

    int32_t FormatMessageW(int32_t flags, intptr_t source, uint32_t messageId, int32_t languageId,
        PS2_WCHAR* buffer, int32_t size, intptr_t* arguments)
    {
        return 0;
    }

    int32_t SetThreadErrorMode(uint32_t newMode, uint32_t* oldMode)
    {
        if (oldMode != NULL)
            *oldMode = 0;
        return 1;
    }

    // ---- file probes (kernel32) ----------------------------------------
    //
    // MonoIO (which these BCL classes also use) is the real file path; the
    // Find/attribute fast paths report absence and the callers fall back.

    void* FindFirstFileExW(PS2_WCHAR* fileName, uint32_t infoLevel, void* findData,
        uint32_t searchOp, intptr_t searchFilter, int32_t additionalFlags)
    {
        return reinterpret_cast<void*>(-1); // INVALID_HANDLE_VALUE
    }

    int32_t GetFileAttributesExW(PS2_WCHAR* fileName, uint32_t infoLevel, void* fileInformation)
    {
        return 0;
    }

    int32_t GetFullPathName(PS2_WCHAR* fileName, int32_t bufferLength, PS2_WCHAR* buffer, intptr_t* filePart)
    {
        // fio paths have no relative form to resolve: identity copy.
        const size_t length = WideLength(fileName);
        if (buffer == NULL || static_cast<size_t>(bufferLength) <= length)
            return static_cast<int32_t>(length + 1); // required size, per API
        memcpy(buffer, fileName, (length + 1) * sizeof(PS2_WCHAR));
        if (filePart != NULL)
            *filePart = 0;
        return static_cast<int32_t>(length);
    }

    // ---- registry (advapi32): uniformly absent -------------------------

    int32_t RegOpenKeyExW(void* key, PS2_WCHAR* subKey, int32_t options, int32_t desired, void** result)
    {
        if (result != NULL)
            *result = NULL;
        return kErrorFileNotFound;
    }

    int32_t RegCloseKey(intptr_t key)
    {
        return 0;
    }

    int32_t RegQueryValueExW(void* key, PS2_WCHAR* valueName, int32_t* reserved, int32_t* type,
        uint8_t* data, int32_t* dataSize)
    {
        return kErrorFileNotFound;
    }

    int32_t RegEnumKeyExW(void* key, int32_t index, PS2_WCHAR* name, int32_t* nameLength, int32_t* reserved,
        PS2_WCHAR* className, int32_t* classLength, int64_t* lastWriteTime)
    {
        return kErrorNoMoreItems;
    }

    int32_t RegQueryInfoKeyW(void* key, PS2_WCHAR* className, int32_t* classLength, intptr_t reserved,
        int32_t* subKeys, int32_t* maxSubKeyLength, int32_t* maxClassLength, int32_t* values,
        int32_t* maxValueNameLength, int32_t* maxValueLength, int32_t* securityDescriptor, int32_t* lastWriteTime)
    {
        if (subKeys != NULL)
            *subKeys = 0;
        if (values != NULL)
            *values = 0;
        return 0;
    }

    // ---- time zone (kernel32 / api-ms-win-core-timezone): UTC ----------

    uint32_t GetTimeZoneInformation(void* timeZoneInformation)
    {
        memset(timeZoneInformation, 0, 172); // TIME_ZONE_INFORMATION: zero bias, no DST
        return kTimeZoneIdUnknown;
    }

    uint32_t GetDynamicTimeZoneInformation(void* dynamicTimeZoneInformation)
    {
        uint8_t* raw = static_cast<uint8_t*>(dynamicTimeZoneInformation);
        memset(raw, 0, 432); // DYNAMIC_TIME_ZONE_INFORMATION
        // TimeZoneKeyName at offset 172; DynamicDaylightTimeDisabled at 428.
        const PS2_WCHAR utc[] = { 'U', 'T', 'C', 0 };
        memcpy(raw + 172, utc, sizeof(utc));
        raw[428] = 1;
        return kTimeZoneIdUnknown;
    }

    int32_t GetTimeZoneInformationForYear(uint16_t year, void* dynamicTimeZoneInformation, void* timeZoneInformation)
    {
        memset(timeZoneInformation, 0, 172);
        return 1; // TRUE: UTC holds for every year
    }

    uint32_t EnumDynamicTimeZoneInformation(uint32_t index, void* dynamicTimeZoneInformation)
    {
        return kErrorNoMoreItems;
    }

    uint32_t GetDynamicTimeZoneInformationEffectiveYears(void* dynamicTimeZoneInformation,
        uint32_t* firstYear, uint32_t* lastYear)
    {
        return kErrorFileNotFound; // caller falls back to the static data
    }

    // ---- crypto (BCrypt) -----------------------------------------------

    uint32_t BCryptGenRandom(intptr_t algorithm, uint8_t* buffer, int32_t count, int32_t flags)
    {
        // Same generator (and the same security caveat) as os::Cryptography.
        il2cpp::os::Cryptography::FillBufferWithRandomBytes(NULL, count, buffer);
        return 0; // STATUS_SUCCESS
    }
}

#endif // IL2CPP_TARGET_PS2
