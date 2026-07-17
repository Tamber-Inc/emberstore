#include "AtomicFile.h"

#include <eacp/Core/Utils/StdPath.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace emberstore
{
namespace
{
namespace fs = std::filesystem;

// Flush the file's bytes to the physical disk before we rename it into place,
// so a power cut can't leave the rename pointing at unwritten data. Best
// effort — a failure here still leaves the atomic rename intact.
void syncFile(const fs::path& file)
{
#if defined(_WIN32)
    auto handle = ::CreateFileW(file.c_str(),
                                GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (handle != INVALID_HANDLE_VALUE)
    {
        ::FlushFileBuffers(handle);
        ::CloseHandle(handle);
    }
#else
    const auto fd = ::open(file.c_str(), O_RDONLY);
    if (fd >= 0)
    {
        ::fsync(fd);
        ::close(fd);
    }
#endif
}

// Flush the directory entry so the rename itself survives a crash. POSIX only;
// on Windows the rename is durable once the file's buffers are flushed.
void syncDirectory(const fs::path& dir)
{
#if !defined(_WIN32)
    const auto fd = ::open(dir.c_str(), O_RDONLY);
    if (fd >= 0)
    {
        ::fsync(fd);
        ::close(fd);
    }
#else
    (void) dir;
#endif
}
} // namespace

std::optional<std::string> readTextFile(const eacp::FilePath& path)
{
    // An fstream over a std::filesystem::path handles Unicode paths on Windows;
    // a UTF-8 fopen would be mangled by the ANSI code page there.
    auto input = std::ifstream {eacp::toStdPath(path), std::ios::binary};
    if (!input.is_open())
        return std::nullopt;

    return std::string {std::istreambuf_iterator<char> {input},
                        std::istreambuf_iterator<char> {}};
}

bool writeFileAtomically(const eacp::FilePath& path,
                         std::string_view bytes,
                         Durability durability)
{
    const auto target = eacp::toStdPath(path);
    const auto directory = target.parent_path();

    auto error = std::error_code {};
    if (!directory.empty())
    {
        fs::create_directories(directory, error);
        if (error)
        {
            std::fprintf(stderr,
                         "emberstore: cannot create %s: %s\n",
                         directory.string().c_str(),
                         error.message().c_str());
            return false;
        }
    }

    // Written beside the target and renamed over it, never straight into it:
    // truncating the target first would empty it before the new bytes landed,
    // and a crash there would lose the data. rename() is atomic — a reader sees
    // the whole old file or the whole new one.
    const auto temp = fs::path {target}.concat(".tmp");

    {
        auto output = std::ofstream {temp, std::ios::binary | std::ios::trunc};
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output)
        {
            std::fprintf(
                stderr, "emberstore: failed writing %s\n", temp.string().c_str());
            auto ignored = std::error_code {};
            fs::remove(temp, ignored);
            return false;
        }
    } // closed before the rename — Windows will not rename an open file.

    if (durability == Durability::Durable)
        syncFile(temp);

    fs::rename(temp, target, error);
    if (error)
    {
        std::fprintf(stderr,
                     "emberstore: cannot replace %s: %s\n",
                     target.string().c_str(),
                     error.message().c_str());
        auto ignored = std::error_code {};
        fs::remove(temp, ignored);
        return false;
    }

    if (durability == Durability::Durable && !directory.empty())
        syncDirectory(directory);

    return true;
}

namespace detail
{
FileStamp stampFile(const eacp::FilePath& path)
{
    const auto target = eacp::toStdPath(path);
    auto error = std::error_code {};

    auto stamp = FileStamp {};
    const auto modified = fs::last_write_time(target, error);
    if (error)
        return {}; // absent or unreadable

    stamp.size = fs::file_size(target, error);
    if (error)
        return {};

    stamp.modifiedTicks =
        static_cast<std::int64_t>(modified.time_since_epoch().count());
    stamp.exists = true;
    return stamp;
}
} // namespace detail
} // namespace emberstore
