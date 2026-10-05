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
    #include <cerrno>
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

namespace emberstore
{
#if !defined(_WIN32)
namespace detail
{
// Defined with stampFile below; AppendFile::stamp shares it.
FileStamp stampFromStat(const struct ::stat& info);
} // namespace detail
#endif

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

AppendFile::~AppendFile() { close(); }

AppendFile::AppendFile(AppendFile&& other) noexcept
    : handle(other.handle)
{
    other.handle = -1;
}

AppendFile& AppendFile::operator=(AppendFile&& other) noexcept
{
    if (this != &other)
    {
        close();
        handle = other.handle;
        other.handle = -1;
    }
    return *this;
}

bool AppendFile::isOpen() const { return handle != -1; }

#if defined(_WIN32)

bool AppendFile::open(const eacp::FilePath& path)
{
    close();

    // FILE_APPEND_DATA makes every WriteFile land at the end of the file, the
    // O_APPEND equivalent. FILE_SHARE_DELETE lets another process replace the
    // file by rename while we hold this handle — the reader-side validation
    // makes that safe, and it is how compaction from a sibling process works.
    auto native =
        ::CreateFileW(path.wide().c_str(),
                      FILE_APPEND_DATA,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      nullptr,
                      OPEN_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL,
                      nullptr);
    if (native == INVALID_HANDLE_VALUE)
    {
        std::fprintf(
            stderr, "emberstore: cannot open %s for appending\n", path.c_str());
        return false;
    }

    handle = reinterpret_cast<std::intptr_t>(native);
    return true;
}

void AppendFile::close()
{
    if (isOpen())
        ::CloseHandle(reinterpret_cast<HANDLE>(handle));
    handle = -1;
}

bool AppendFile::append(std::string_view bytes, Durability durability)
{
    if (!isOpen())
        return false;

    auto* native = reinterpret_cast<HANDLE>(handle);
    auto written = DWORD {0};

    if (!::WriteFile(native,
                     bytes.data(),
                     static_cast<DWORD>(bytes.size()),
                     &written,
                     nullptr)
        || written != bytes.size())
    {
        std::fprintf(stderr, "emberstore: append failed\n");
        return false;
    }

    if (durability == Durability::Durable)
        ::FlushFileBuffers(native);

    return true;
}

detail::FileStamp AppendFile::stamp() const
{
    if (!isOpen())
        return {};

    auto info = BY_HANDLE_FILE_INFORMATION {};
    if (!::GetFileInformationByHandle(reinterpret_cast<HANDLE>(handle), &info))
        return {};

    auto stamp = detail::FileStamp {};
    stamp.exists = true;
    stamp.size =
        (static_cast<std::uintmax_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    stamp.modifiedTicks =
        (static_cast<std::int64_t>(info.ftLastWriteTime.dwHighDateTime) << 32)
        | info.ftLastWriteTime.dwLowDateTime;
    return stamp;
}

#else

bool AppendFile::open(const eacp::FilePath& path)
{
    close();

    // O_CLOEXEC for the same reason the IPC lock uses it: a spawned child
    // must not inherit (and keep alive) our write handle.
    const auto fd =
        ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        std::fprintf(
            stderr, "emberstore: cannot open %s for appending\n", path.c_str());
        return false;
    }

    handle = fd;
    return true;
}

void AppendFile::close()
{
    if (isOpen())
        ::close(static_cast<int>(handle));
    handle = -1;
}

bool AppendFile::append(std::string_view bytes, Durability durability)
{
    if (!isOpen())
        return false;

    const auto fd = static_cast<int>(handle);
    auto remaining = bytes;

    while (!remaining.empty())
    {
        const auto written = ::write(fd, remaining.data(), remaining.size());
        if (written < 0)
        {
            if (errno == EINTR)
                continue;
            std::fprintf(stderr, "emberstore: append failed\n");
            return false;
        }
        remaining.remove_prefix(static_cast<std::size_t>(written));
    }

    if (durability == Durability::Durable)
        ::fsync(fd);

    return true;
}

detail::FileStamp AppendFile::stamp() const
{
    if (!isOpen())
        return {};

    struct ::stat info {};
    if (::fstat(static_cast<int>(handle), &info) != 0)
        return {};

    return detail::stampFromStat(info);
}

#endif

void adoptLegacyFile(const eacp::FilePath& legacy, const eacp::FilePath& target)
{
    const auto from = eacp::toStdPath(legacy);
    const auto to = eacp::toStdPath(target);

    auto error = std::error_code {};
    if (fs::exists(to, error) || !fs::exists(from, error))
        return;

    fs::rename(from, to, error);
    if (error)
        std::fprintf(stderr,
                     "emberstore: cannot adopt %s as %s: %s\n",
                     from.string().c_str(),
                     to.string().c_str(),
                     error.message().c_str());
}

namespace detail
{
// One stat syscall per stamp — this runs on every cached read, so the
// two-call std::filesystem version (last_write_time + file_size, a stat
// each) costs double for nothing.
#if defined(_WIN32)

FileStamp stampFile(const eacp::FilePath& path)
{
    auto data = WIN32_FILE_ATTRIBUTE_DATA {};
    if (!::GetFileAttributesExW(path.wide().c_str(), GetFileExInfoStandard, &data))
        return {}; // absent or unreadable

    auto stamp = FileStamp {};
    stamp.exists = true;
    stamp.size =
        (static_cast<std::uintmax_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    stamp.modifiedTicks =
        (static_cast<std::int64_t>(data.ftLastWriteTime.dwHighDateTime) << 32)
        | data.ftLastWriteTime.dwLowDateTime;
    return stamp;
}

#else

FileStamp stampFromStat(const struct ::stat& info)
{
    #if defined(__APPLE__)
    const auto& modified = info.st_mtimespec;
    #else
    const auto& modified = info.st_mtim;
    #endif

    auto stamp = FileStamp {};
    stamp.exists = true;
    stamp.size = static_cast<std::uintmax_t>(info.st_size);
    stamp.modifiedTicks = static_cast<std::int64_t>(modified.tv_sec) * 1'000'000'000
                          + static_cast<std::int64_t>(modified.tv_nsec);
    return stamp;
}

FileStamp stampFile(const eacp::FilePath& path)
{
    struct ::stat info {};
    if (::stat(path.c_str(), &info) != 0)
        return {}; // absent or unreadable

    return stampFromStat(info);
}

#endif
} // namespace detail
} // namespace emberstore
