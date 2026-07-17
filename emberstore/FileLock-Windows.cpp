#include "FileLock.h"

#include <eacp/Core/Utils/StdPath.h>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>
#include <system_error>
#include <utility>

namespace emberstore
{
namespace fs = std::filesystem;

// LockFileEx() byte-range locks belong to the file handle, so two handles on
// the same file contend whether they come from one process or two — and Windows
// drops the lock when the handle closes, including on a crash. The whole range
// is locked (MAXDWORD:MAXDWORD) since the file's contents are irrelevant.
struct FileLock::Impl
{
    explicit Impl(fs::path pathToUse) : path(std::move(pathToUse)) {}

    ~Impl()
    {
        unlock();
        if (handle != INVALID_HANDLE_VALUE)
            ::CloseHandle(handle);
    }

    // Deferred so that constructing a FileLock never touches the filesystem.
    bool open()
    {
        if (handle != INVALID_HANDLE_VALUE)
            return true;

        auto error = std::error_code {};
        const auto directory = path.parent_path();
        if (!directory.empty())
            fs::create_directories(directory, error);

        handle = ::CreateFileW(path.wstring().c_str(),
                               GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return handle != INVALID_HANDLE_VALUE;
    }

    bool lock()
    {
        if (held)
            return true;
        if (!open())
            return false;

        auto overlapped = OVERLAPPED {};
        held = ::LockFileEx(handle,
                            LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                            0, MAXDWORD, MAXDWORD, &overlapped)
            != FALSE;
        return held;
    }

    void unlock()
    {
        if (!held)
            return;

        auto overlapped = OVERLAPPED {};
        ::UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &overlapped);
        held = false;
    }

    fs::path path;
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool held = false;
};

FileLock::FileLock(const eacp::FilePath& path)
    : impl(std::make_unique<Impl>(eacp::toStdPath(path)))
{
}

FileLock::~FileLock() = default;
FileLock::FileLock(FileLock&&) noexcept = default;
FileLock& FileLock::operator=(FileLock&&) noexcept = default;

bool FileLock::tryAcquire() { return impl->lock(); }
void FileLock::release() { impl->unlock(); }
bool FileLock::isHeld() const { return impl->held; }
} // namespace emberstore
