#include "FileLock.h"

#include <eacp/Core/Utils/StdPath.h>

#include <filesystem>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace emberstore
{
namespace fs = std::filesystem;

// flock() locks belong to the open file description, so two descriptors on the
// same file contend whether they come from one process or two — and the kernel
// drops the lock when the descriptor closes, including on a crash.
struct FileLock::Impl
{
    explicit Impl(fs::path pathToUse) : path(std::move(pathToUse)) {}

    ~Impl()
    {
        unlock();
        if (descriptor >= 0)
            ::close(descriptor);
    }

    // Deferred so that constructing a FileLock never touches the filesystem.
    bool open()
    {
        if (descriptor >= 0)
            return true;

        auto error = std::error_code {};
        const auto directory = path.parent_path();
        if (!directory.empty())
            fs::create_directories(directory, error);

        descriptor = ::open(path.c_str(), O_CREAT | O_RDWR, 0666);
        return descriptor >= 0;
    }

    bool lock()
    {
        if (held)
            return true;
        if (!open())
            return false;

        held = ::flock(descriptor, LOCK_EX | LOCK_NB) == 0;
        return held;
    }

    void unlock()
    {
        if (!held)
            return;
        ::flock(descriptor, LOCK_UN);
        held = false;
    }

    fs::path path;
    int descriptor = -1;
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
