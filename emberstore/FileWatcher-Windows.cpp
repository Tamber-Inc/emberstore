#include "FileWatcher.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/StdPath.h>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>

// Watches the parent directory with ReadDirectoryChangesW (overlapped) on a
// worker thread and marshals matching changes to the main thread via callAsync,
// so the contract matches the macOS backend: onChange runs on the message
// thread.
namespace emberstore
{
namespace fs = std::filesystem;

struct FileWatcher::Impl
{
    Impl(const fs::path& path, Callback onChangeToUse)
        : onChange(std::move(onChangeToUse))
        , filename(path.filename().wstring())
        , alive(std::make_shared<int>(0))
    {
        auto directory = path.parent_path();
        auto error = std::error_code {};
        fs::create_directories(directory, error);

        directoryHandle =
            ::CreateFileW(directory.wstring().c_str(),
                          FILE_LIST_DIRECTORY,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr,
                          OPEN_EXISTING,
                          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                          nullptr);
        if (directoryHandle == INVALID_HANDLE_VALUE)
            return;

        stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (stopEvent == nullptr)
        {
            ::CloseHandle(directoryHandle);
            directoryHandle = INVALID_HANDLE_VALUE;
            return;
        }

        worker = std::thread([this] { run(); });
    }

    ~Impl()
    {
        if (directoryHandle == INVALID_HANDLE_VALUE)
            return;
        ::SetEvent(stopEvent);
        ::CancelIoEx(directoryHandle, nullptr);
        if (worker.joinable())
            worker.join();
        ::CloseHandle(stopEvent);
        ::CloseHandle(directoryHandle);
    }

    void run()
    {
        auto overlapped = OVERLAPPED {};
        overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const std::weak_ptr<int> guard = alive;

        alignas(DWORD) unsigned char buffer[64 * 1024];
        constexpr DWORD filter = FILE_NOTIFY_CHANGE_LAST_WRITE
                                 | FILE_NOTIFY_CHANGE_FILE_NAME
                                 | FILE_NOTIFY_CHANGE_SIZE;

        for (;;)
        {
            ::ResetEvent(overlapped.hEvent);
            if (!::ReadDirectoryChangesW(directoryHandle,
                                         buffer,
                                         sizeof buffer,
                                         FALSE,
                                         filter,
                                         nullptr,
                                         &overlapped,
                                         nullptr))
                break;

            HANDLE handles[] = {stopEvent, overlapped.hEvent};
            if (::WaitForMultipleObjects(2, handles, FALSE, INFINITE)
                == WAIT_OBJECT_0)
                break; // stopEvent

            auto bytes = DWORD {0};
            if (!::GetOverlappedResult(directoryHandle, &overlapped, &bytes, TRUE)
                || bytes == 0)
                continue;

            if (matchesWatchedFile(buffer))
                eacp::Threads::callAsync(
                    [this, guard]
                    {
                        if (!guard.expired())
                            onChange();
                    });
        }

        ::CloseHandle(overlapped.hEvent);
    }

    bool matchesWatchedFile(const unsigned char* buffer) const
    {
        const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer);
        for (;;)
        {
            const auto name =
                std::wstring {info->FileName, info->FileNameLength / sizeof(WCHAR)};
            if (name == filename)
                return true;
            if (info->NextEntryOffset == 0)
                return false;
            info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(
                reinterpret_cast<const unsigned char*>(info)
                + info->NextEntryOffset);
        }
    }

    Callback onChange;
    std::wstring filename;
    std::shared_ptr<int> alive;
    HANDLE directoryHandle = INVALID_HANDLE_VALUE;
    HANDLE stopEvent = nullptr;
    std::thread worker;
};

FileWatcher::FileWatcher(const eacp::FilePath& path, Callback onChange)
    : impl(std::make_unique<Impl>(eacp::toStdPath(path), std::move(onChange)))
{
}

FileWatcher::~FileWatcher() = default;
} // namespace emberstore
