#include "FileWatcher.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/StdPath.h>

#include <sys/inotify.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>

// Watches the parent directory with inotify on a worker thread and marshals
// matching changes to the main thread via callAsync, so the contract matches
// the macOS and Windows backends: onChange runs on the message thread.
//
// The watch is on the directory rather than the file so it survives the atomic
// temp + rename saves our own writer and editors like nvim use — a watch on the
// inode would go stale the moment the file is replaced. That is also why
// IN_MOVED_TO is in the mask: a rename-into-place delivers only that.
namespace emberstore
{
namespace fs = std::filesystem;

struct FileWatcher::Impl
{
    Impl(const fs::path& path, Callback onChangeToUse)
        : onChange(std::move(onChangeToUse))
        , filename(path.filename().string())
        , alive(std::make_shared<int>(0))
    {
        auto directory = path.parent_path();
        auto error = std::error_code {};
        fs::create_directories(directory, error);

        inotifyFd = ::inotify_init1(IN_CLOEXEC);

        if (inotifyFd < 0)
            return;

        // Everything that can end with the watched name holding new bytes. The
        // writer's rename lands as IN_MOVED_TO; an in-place write ends at
        // IN_CLOSE_WRITE; IN_MODIFY covers a writer that never closes.
        constexpr auto mask = std::uint32_t {IN_CLOSE_WRITE | IN_MOVED_TO
                                             | IN_MOVED_FROM | IN_CREATE
                                             | IN_DELETE | IN_MODIFY};

        watch = ::inotify_add_watch(inotifyFd, directory.c_str(), mask);

        if (watch < 0)
        {
            ::close(inotifyFd);
            inotifyFd = -1;
            return;
        }

        stopFd = ::eventfd(0, EFD_CLOEXEC);

        if (stopFd < 0)
        {
            ::close(inotifyFd);
            inotifyFd = -1;
            return;
        }

        worker = std::thread([this] { run(); });
    }

    ~Impl()
    {
        if (inotifyFd < 0)
            return;

        const auto wake = std::uint64_t {1};
        const auto written = ::write(stopFd, &wake, sizeof wake);
        static_cast<void>(written);

        if (worker.joinable())
            worker.join();

        ::close(stopFd);
        ::close(inotifyFd);
    }

    void run()
    {
        const std::weak_ptr<int> guard = alive;

        // inotify requires a buffer large enough for one event plus its name,
        // and is happier with room for a burst of them.
        alignas(inotify_event) std::array<char, 64 * 1024> buffer {};

        for (;;)
        {
            auto fds = std::array<pollfd, 2> {
                pollfd {stopFd, POLLIN, 0},
                pollfd {inotifyFd, POLLIN, 0},
            };

            if (::poll(fds.data(), fds.size(), -1) < 0)
            {
                if (errno == EINTR)
                    continue;

                break;
            }

            if ((fds[0].revents & POLLIN) != 0)
                break;

            if ((fds[1].revents & POLLIN) == 0)
                continue;

            const auto bytes = ::read(inotifyFd, buffer.data(), buffer.size());

            if (bytes <= 0)
            {
                if (bytes < 0 && errno == EINTR)
                    continue;

                break;
            }

            if (matchesWatchedFile(buffer.data(), (std::size_t) bytes))
                eacp::Threads::callAsync(
                    [this, guard]
                    {
                        if (!guard.expired())
                            onChange();
                    });
        }
    }

    bool matchesWatchedFile(const char* buffer, std::size_t bytes) const
    {
        for (auto offset = std::size_t {0}; offset + sizeof(inotify_event) <= bytes;)
        {
            const auto* event =
                reinterpret_cast<const inotify_event*>(buffer + offset);

            // len includes the padding NULs inotify appends, so compare against
            // the C string rather than a string built from the whole field.
            if (event->len > 0 && filename == event->name)
                return true;

            offset += sizeof(inotify_event) + event->len;
        }

        return false;
    }

    Callback onChange;
    std::string filename;
    std::shared_ptr<int> alive;
    int inotifyFd = -1;
    int stopFd = -1;
    int watch = -1;
    std::thread worker;
};

FileWatcher::FileWatcher(const eacp::FilePath& path, Callback onChange)
    : impl(std::make_unique<Impl>(eacp::toStdPath(path), std::move(onChange)))
{
}

FileWatcher::~FileWatcher() = default;
} // namespace emberstore
