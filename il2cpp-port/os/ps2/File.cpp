// os::File and os::Directory for the PS2 EE (plan section 11.2).
//
// Backed by the LEGACY fio RPC (fioOpen and friends), not fileXio: newlib's
// file descriptors route through fileXio, which PCSX2's host: HLE does not
// implement -- fio is the path the emulator (and a real console with ps2link)
// actually serves. See docs/notes/verify-log.md, M5. Declared as externs so we
// do not depend on ps2sdk headers being on the il2cpp include path.
//
// Single-threaded (IL2CPP_SUPPORT_THREADS=0), so no locking anywhere here.
#include "il2cpp-config.h"

#if IL2CPP_TARGET_PS2

#include <string>
#include <string.h>
#include <stdio.h>

#include "os/File.h"
#include "os/Directory.h"
#include "os/ErrorCodes.h"
#include "utils/Expected.h"
#include "utils/Il2CppError.h"
#include "utils/StringView.h"

extern "C"
{
    int fioOpen(const char* name, int mode);
    int fioClose(int fd);
    int fioRead(int fd, void* buf, int size);
    int fioWrite(int fd, const void* buf, int size);
    int fioLseek(int fd, int offset, int whence);
    int fioMkdir(const char* name);
    int fioRmdir(const char* name);
    int fioRemove(const char* name);
}

// Values from ps2sdk common/include/io_common.h (stable RPC ABI, restated
// here so this file compiles without ps2sdk on the include path).
#define PS2_FIO_O_RDONLY 0x0001
#define PS2_FIO_O_WRONLY 0x0002
#define PS2_FIO_O_RDWR   0x0003
#define PS2_FIO_O_APPEND 0x0100
#define PS2_FIO_O_CREAT  0x0200
#define PS2_FIO_O_TRUNC  0x0400

namespace il2cpp
{
namespace os
{
    // Opaque to everyone else; ours to define. fd < 0 marks the three
    // standard handles (-1 stdin, -2 stdout, -3 stderr), which do not exist
    // as fio descriptors and are served by the EE kernel TTY instead.
    struct FileHandle
    {
        int fd;
    };

    static FileHandle s_StdIn = { -1 };
    static FileHandle s_StdOut = { -2 };
    static FileHandle s_StdErr = { -3 };

    static bool IsStdHandle(FileHandle* h)
    {
        return h == &s_StdIn || h == &s_StdOut || h == &s_StdErr;
    }

    FileHandle* File::GetStdInput()
    {
        return &s_StdIn;
    }

    FileHandle* File::GetStdOutput()
    {
        return &s_StdOut;
    }

    FileHandle* File::GetStdError()
    {
        return &s_StdErr;
    }

    utils::Expected<bool> File::Isatty(FileHandle* fileHandle)
    {
        // The EE kernel TTY is write-only and has no termios: claiming it is
        // a terminal sends Mono's Console down the TermInfoDriver path
        // (termcap files, TERM variable), none of which exists here. "Not a
        // tty" selects the plain stream path, which is what the device is.
        return false;
    }

    utils::Expected<bool> File::CreatePipe(FileHandle** read_handle, FileHandle** write_handle)
    {
        return false;
    }

    utils::Expected<bool> File::CreatePipe(FileHandle** read_handle, FileHandle** write_handle, int* error)
    {
        *error = kErrorCodeGenFailure;
        return false;
    }

    FileType File::GetFileType(FileHandle* handle)
    {
        return IsStdHandle(handle) ? kFileTypeChar : kFileTypeDisk;
    }

    FileHandle* File::Open(const std::string& path, int openMode, int accessMode, int shareMode, int options, int* error)
    {
        int flags = 0;
        switch (accessMode)
        {
            case kFileAccessRead:
                flags = PS2_FIO_O_RDONLY;
                break;
            case kFileAccessWrite:
                flags = PS2_FIO_O_WRONLY;
                break;
            default:
                flags = PS2_FIO_O_RDWR;
                break;
        }
        switch (openMode)
        {
            case kFileModeCreateNew: // no O_EXCL over fio; approximated
            case kFileModeCreate:
                flags |= PS2_FIO_O_CREAT | PS2_FIO_O_TRUNC;
                break;
            case kFileModeOpen:
                break;
            case kFileModeOpenOrCreate:
                flags |= PS2_FIO_O_CREAT;
                break;
            case kFileModeTruncate:
                flags |= PS2_FIO_O_TRUNC;
                break;
            case kFileModeAppend:
                flags |= PS2_FIO_O_CREAT | PS2_FIO_O_APPEND;
                break;
        }

        const int fd = fioOpen(path.c_str(), flags);
        if (fd < 0)
        {
            *error = kErrorCodeFileNotFound;
            return NULL;
        }

        FileHandle* handle = new FileHandle();
        handle->fd = fd;
        *error = kErrorCodeSuccess;
        return handle;
    }

    bool File::Close(FileHandle* handle, int* error)
    {
        *error = kErrorCodeSuccess;
        if (handle == NULL || IsStdHandle(handle))
            return true;
        fioClose(handle->fd);
        delete handle;
        return true;
    }

    int File::Read(FileHandle* handle, char* dest, int count, int* error)
    {
        *error = kErrorCodeSuccess;
        if (handle == NULL || IsStdHandle(handle))
            return 0; // no interactive stdin on the console
        const int n = fioRead(handle->fd, dest, count);
        if (n < 0)
        {
            *error = kErrorCodeGenFailure;
            return 0;
        }
        return n;
    }

    int32_t File::Write(FileHandle* handle, const char* buffer, int count, int* error)
    {
        *error = kErrorCodeSuccess;
        if (handle == &s_StdOut || handle == &s_StdErr)
        {
            // EE kernel TTY: shows up in the PCSX2 console, which is what the
            // run harness parses for tokens. fwrite keeps ordering with the
            // runtime's own printf output.
            fwrite(buffer, 1, static_cast<size_t>(count), handle == &s_StdErr ? stderr : stdout);
            fflush(handle == &s_StdErr ? stderr : stdout);
            return count;
        }
        if (handle == NULL || handle == &s_StdIn)
        {
            *error = kErrorCodeInvalidHandle;
            return -1;
        }
        const int n = fioWrite(handle->fd, buffer, count);
        if (n < 0)
        {
            *error = kErrorCodeGenFailure;
            return -1;
        }
        return n;
    }

    int64_t File::Seek(FileHandle* handle, int64_t offset, int origin, int* error)
    {
        // SeekOrigin Begin/Current/End == fio whence 0/1/2.
        *error = kErrorCodeSuccess;
        if (handle == NULL || IsStdHandle(handle))
        {
            *error = kErrorCodeInvalidHandle;
            return -1;
        }
        const int pos = fioLseek(handle->fd, static_cast<int>(offset), origin);
        if (pos < 0)
        {
            *error = kErrorCodeGenFailure;
            return -1;
        }
        return pos;
    }

    bool File::Flush(FileHandle* handle, int* error)
    {
        *error = kErrorCodeSuccess;
        return true; // fio writes are synchronous RPCs; nothing is buffered
    }

    int64_t File::GetLength(FileHandle* handle, int* error)
    {
        *error = kErrorCodeSuccess;
        if (handle == NULL || IsStdHandle(handle))
        {
            *error = kErrorCodeInvalidHandle;
            return -1;
        }
        const int pos = fioLseek(handle->fd, 0, 1 /*current*/);
        const int end = fioLseek(handle->fd, 0, 2 /*end*/);
        fioLseek(handle->fd, pos, 0 /*begin*/);
        if (end < 0)
        {
            *error = kErrorCodeGenFailure;
            return -1;
        }
        return end;
    }

    bool File::SetLength(FileHandle* handle, int64_t length, int* error)
    {
        // fio has no truncate-to-size. Only the no-op case can be honest.
        int e = kErrorCodeSuccess;
        if (GetLength(handle, &e) == length && e == kErrorCodeSuccess)
        {
            *error = kErrorCodeSuccess;
            return true;
        }
        *error = kErrorCodeGenFailure;
        return false;
    }

    // Truncate is supplied by os/Generic/File.cpp (defers to SetLength).

    bool File::SetFileTime(FileHandle* handle, int64_t creation_time, int64_t last_access_time, int64_t last_write_time, int* error)
    {
        // No timestamp support over fio; the times are dropped. Reported as
        // success because the BCL treats failure here as fatal for file
        // creation, and dropped metadata is the documented platform behavior.
        *error = kErrorCodeSuccess;
        return true;
    }

    void File::Lock(FileHandle* handle, int64_t position, int64_t length, int* error)
    {
        *error = kErrorCodeSuccess; // single process, single thread
    }

    void File::Unlock(FileHandle* handle, int64_t position, int64_t length, int* error)
    {
        *error = kErrorCodeSuccess;
    }

    bool File::Cancel(FileHandle* handle)
    {
        return false;
    }

    utils::Expected<bool> File::DuplicateHandle(FileHandle* source_process_handle, FileHandle* source_handle,
        FileHandle* target_process_handle, FileHandle** target_handle, int access, int inherit, int options, int* error)
    {
        // One process, one thread: the same handle serves. Refuse for real
        // descriptors, where two owners would double-close.
        if (IsStdHandle(source_handle))
        {
            *target_handle = source_handle;
            *error = kErrorCodeSuccess;
            return true;
        }
        *error = kErrorCodeGenFailure;
        return false;
    }

    utils::Expected<bool> File::IsExecutable(const std::string& path)
    {
        return false;
    }

    UnityPalFileAttributes File::GetFileAttributes(const std::string& path, int* error)
    {
        const int fd = fioOpen(path.c_str(), PS2_FIO_O_RDONLY);
        if (fd >= 0)
        {
            fioClose(fd);
            *error = kErrorCodeSuccess;
            return kFileAttributeNormal;
        }
        *error = kErrorCodeFileNotFound;
        return static_cast<UnityPalFileAttributes>(-1);
    }

    bool File::SetFileAttributes(const std::string& path, UnityPalFileAttributes attributes, int* error)
    {
        *error = kErrorCodeSuccess; // attributes do not exist on this fs
        return true;
    }

    bool File::GetFileStat(const std::string& path, FileStat* stat, int* error)
    {
        const int fd = fioOpen(path.c_str(), PS2_FIO_O_RDONLY);
        if (fd < 0)
        {
            *error = kErrorCodeFileNotFound;
            return false;
        }
        const int end = fioLseek(fd, 0, 2 /*end*/);
        fioClose(fd);

        const size_t slash = path.find_last_of("/\\:");
        stat->name = slash == std::string::npos ? path : path.substr(slash + 1);
        stat->attributes = kFileAttributeNormal;
        stat->length = end < 0 ? 0 : end;
        stat->creation_time = 0;
        stat->last_access_time = 0;
        stat->last_write_time = 0;
        *error = kErrorCodeSuccess;
        return true;
    }

    bool File::CopyFile(const std::string& src, const std::string& dest, bool overwrite, int* error)
    {
        const int in = fioOpen(src.c_str(), PS2_FIO_O_RDONLY);
        if (in < 0)
        {
            *error = kErrorCodeFileNotFound;
            return false;
        }
        if (!overwrite)
        {
            const int probe = fioOpen(dest.c_str(), PS2_FIO_O_RDONLY);
            if (probe >= 0)
            {
                fioClose(probe);
                fioClose(in);
                *error = kErrorCodeGenFailure;
                return false;
            }
        }
        const int out = fioOpen(dest.c_str(), PS2_FIO_O_WRONLY | PS2_FIO_O_CREAT | PS2_FIO_O_TRUNC);
        if (out < 0)
        {
            fioClose(in);
            *error = kErrorCodeAccessDenied;
            return false;
        }

        static char buffer[16 * 1024]; // static: keep this off the 8K stacks
        bool ok = true;
        for (;;)
        {
            const int n = fioRead(in, buffer, sizeof(buffer));
            if (n < 0)
                ok = false;
            if (n <= 0)
                break;
            if (fioWrite(out, buffer, n) != n)
            {
                ok = false;
                break;
            }
        }
        fioClose(in);
        fioClose(out);
        *error = ok ? kErrorCodeSuccess : kErrorCodeGenFailure;
        return ok;
    }

    bool File::DeleteFile(const std::string& path, int* error)
    {
        if (fioRemove(path.c_str()) < 0)
        {
            *error = kErrorCodeFileNotFound;
            return false;
        }
        *error = kErrorCodeSuccess;
        return true;
    }

    bool File::MoveFile(const std::string& src, const std::string& dest, int* error)
    {
        // No rename in the fio RPC set: copy then delete.
        if (!CopyFile(src, dest, false, error))
            return false;
        return DeleteFile(src, error);
    }

    bool File::ReplaceFile(const std::string& sourceFileName, const std::string& destinationFileName,
        const std::string& destinationBackupFileName, bool ignoreMetadataErrors, int* error)
    {
        if (!destinationBackupFileName.empty())
        {
            if (!CopyFile(destinationFileName, destinationBackupFileName, true, error))
                return false;
        }
        int ignored = kErrorCodeSuccess;
        DeleteFile(destinationFileName, &ignored);
        return MoveFile(sourceFileName, destinationFileName, error);
    }

    // ---- Directory ------------------------------------------------------

    std::string Directory::GetCurrent(int* error)
    {
        *error = kErrorCodeSuccess;
        return "host:";
    }

    bool Directory::SetCurrent(const std::string& path, int* error)
    {
        *error = kErrorCodeSuccess; // fio paths are absolute; nothing to set
        return true;
    }

    bool Directory::Create(const std::string& path, int* error)
    {
        if (fioMkdir(path.c_str()) < 0)
        {
            *error = kErrorCodeGenFailure;
            return false;
        }
        *error = kErrorCodeSuccess;
        return true;
    }

    bool Directory::Remove(const std::string& path, int* error)
    {
        if (fioRmdir(path.c_str()) < 0)
        {
            *error = kErrorCodeGenFailure;
            return false;
        }
        *error = kErrorCodeSuccess;
        return true;
    }

    // Directory enumeration is not served by PCSX2's host: HLE (dopen exists
    // in the protocol but the M6 gate does not need listings). Every search
    // completes immediately with no entries; File.Exists style probes go
    // through GetFileAttributes above, which does work.
    Directory::FindHandle::FindHandle(const utils::StringView<Il2CppNativeChar>& searchPathWithPattern)
        : osHandle(NULL), handleFlags(kNoFindHandleFlags)
    {
        const std::string full(searchPathWithPattern.Str(), searchPathWithPattern.Length());
        const size_t slash = full.find_last_of("/\\");
        if (slash == std::string::npos)
        {
            directoryPath = std::string();
            pattern = full;
        }
        else
        {
            directoryPath = full.substr(0, slash);
            pattern = full.substr(slash + 1);
        }
    }

    Directory::FindHandle::~FindHandle()
    {
        CloseOSHandle();
    }

    int32_t Directory::FindHandle::CloseOSHandle()
    {
        osHandle = NULL;
        return kErrorCodeSuccess;
    }

    os::ErrorCode Directory::FindFirstFile(FindHandle* findHandle, const utils::StringView<Il2CppNativeChar>& searchPathWithPattern,
        Il2CppNativeString* resultFileName, int32_t* resultAttributes)
    {
        return kErrorCodeNoMoreFiles;
    }

    os::ErrorCode Directory::FindNextFile(FindHandle* findHandle, Il2CppNativeString* resultFileName, int32_t* resultAttributes)
    {
        return kErrorCodeNoMoreFiles;
    }

    int32_t Directory::CloseOSFindHandleDirectly(intptr_t osHandle)
    {
        return kErrorCodeSuccess;
    }
}
}

#endif // IL2CPP_TARGET_PS2
