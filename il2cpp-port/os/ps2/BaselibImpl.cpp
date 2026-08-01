// The baselib platform functions il2cpp actually links against on the EE
// (plan section 11.2). Baselib proper is header-only in this build; the
// twelve out-of-line entry points below are the whole platform contract for
// a single-threaded target:
//
//  - SystemSemaphore backs Cpp/ReentrantLock. With one thread there is never
//    contention, so a plain counter with non-blocking Acquire is faithful:
//    a blocked Acquire could only ever be a self-deadlock.
//  - DynamicLibrary always fails: the ELF is one static image.
//  - Thread identity is constant.
#include "il2cpp-config.h"

#if IL2CPP_TARGET_PS2

#include <stdio.h>
#include <string.h>

#include "Baselib.h"
#include "C/Baselib_SystemSemaphore.h"
#include "C/Baselib_DynamicLibrary.h"
#include "C/Baselib_ErrorState.h"
#include "C/Baselib_Thread.h"

namespace il2cpp_baselib
{
    // ---- SystemSemaphore (counter at the caller-provided address; matches
    // Baselib_SystemSemaphore_PlatformSize == 4 in our platform header) ----

    static int32_t* Counter(Baselib_SystemSemaphore_Handle semaphore)
    {
        return static_cast<int32_t*>(semaphore.handle);
    }

    Baselib_SystemSemaphore_Handle Baselib_SystemSemaphore_CreateInplace(void* semaphoreData)
    {
        *static_cast<int32_t*>(semaphoreData) = 0;
        Baselib_SystemSemaphore_Handle handle = { semaphoreData };
        return handle;
    }

    void Baselib_SystemSemaphore_FreeInplace(Baselib_SystemSemaphore_Handle semaphore)
    {
    }

    void Baselib_SystemSemaphore_Acquire(Baselib_SystemSemaphore_Handle semaphore)
    {
        if (*Counter(semaphore) > 0)
            --*Counter(semaphore);
        // else: with one thread, waiting could never be satisfied; proceed.
    }

    bool Baselib_SystemSemaphore_TryAcquire(Baselib_SystemSemaphore_Handle semaphore)
    {
        if (*Counter(semaphore) > 0)
        {
            --*Counter(semaphore);
            return true;
        }
        return false;
    }

    bool Baselib_SystemSemaphore_TryTimedAcquire(Baselib_SystemSemaphore_Handle semaphore, uint32_t timeoutInMilliseconds)
    {
        return Baselib_SystemSemaphore_TryAcquire(semaphore);
    }

    void Baselib_SystemSemaphore_Release(Baselib_SystemSemaphore_Handle semaphore, uint32_t count)
    {
        *Counter(semaphore) += static_cast<int32_t>(count);
    }

    // ---- DynamicLibrary ------------------------------------------------

    static void RaiseNotSupported(Baselib_ErrorState* errorState)
    {
        if (errorState != NULL && errorState->code == Baselib_ErrorCode_Success)
        {
            errorState->code = Baselib_ErrorCode_NotSupported;
            errorState->nativeErrorCode = 0;
        }
    }

    Baselib_DynamicLibrary_Handle Baselib_DynamicLibrary_OpenUtf8(const char* pathnameUtf8, Baselib_ErrorState* errorState)
    {
        RaiseNotSupported(errorState);
        return Baselib_DynamicLibrary_Handle_Invalid;
    }

    void* Baselib_DynamicLibrary_GetFunction(Baselib_DynamicLibrary_Handle handle, const char* functionName, Baselib_ErrorState* errorState)
    {
        RaiseNotSupported(errorState);
        return NULL;
    }

    void Baselib_DynamicLibrary_Close(Baselib_DynamicLibrary_Handle handle)
    {
    }

    // ---- ErrorState / Thread -------------------------------------------

    uint32_t Baselib_ErrorState_Explain(const Baselib_ErrorState* errorState, char* buffer, uint32_t bufferLen, Baselib_ErrorState_ExplainVerbosity verbosity)
    {
        if (errorState == NULL || buffer == NULL || bufferLen == 0)
            return 0;
        const int written = snprintf(buffer, bufferLen, "baselib error %d", static_cast<int>(errorState->code));
        return written < 0 ? 0 : static_cast<uint32_t>(written);
    }

    Baselib_Thread_Id Baselib_Thread_GetCurrentThreadId(void)
    {
        return static_cast<Baselib_Thread_Id>(1);
    }

    void Baselib_Thread_YieldExecution(void)
    {
    }
}

#endif // IL2CPP_TARGET_PS2
