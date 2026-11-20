#pragma once

#include <eacp/Core/Utils/FilePath.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace emberstore
{
// How hard a write tries to survive a power loss.
//
//   Atomic — write a sibling temp file and rename() it over the target. The
//     rename is atomic, so a reader (or a crash) ever sees the whole old file
//     or the whole new one, never a half-written or truncated one. This alone
//     is what stops "crashed mid-save" from wiping a user's data.
//
//   Durable — Atomic, plus fsync the data before the rename and fsync the
//     directory after it, so the bytes and the rename itself outlive an OS
//     crash or power cut. Costs a few milliseconds per write.
enum class Durability
{
    Atomic,
    Durable,
};

// The on-disk encoding a store writes. Reads sniff the file's actual content
// (a Binary Document can read a JSON file and vice versa), but the format is
// fixed per Database: it picks the file extension, so a store opened under
// the other format is a different file.
//
//   Json — pretty-printed text: greppable, diffable, hand-editable.
//   Binary — Calder's packed format ("CLD1"): a fraction of the size,
//     several times faster to encode/decode, and floats round-trip bit-exact
//     instead of via decimal text.
enum class Format
{
    Json,
    Binary,
};

// nullopt when the file is absent or unreadable (an empty file reads as "").
std::optional<std::string> readTextFile(const eacp::FilePath& path);

// Writes bytes to path via a temp sibling + rename (see Durability). Creates
// parent directories. Returns false and logs on any IO failure — the target
// file is left untouched, never partially overwritten.
//
// Free of Document<T>'s threading rules: no cache, no lock, no message-thread
// assert. Callers that persist off-thread use this directly.
bool writeFileAtomically(const eacp::FilePath& path,
                         std::string_view bytes,
                         Durability durability);

namespace detail
{
// A cheap identity for a file's contents — which file (device + inode, or
// volume serial + file index on Windows) plus last-write time and size. Two
// absent files compare equal. Lets a cached parse be validated with a stat
// instead of re-reading and re-parsing the file. The file identity is what
// catches a same-size replace-by-rename within one mtime tick. Only ever
// compared against other stamps from this process, so the units are whatever
// the platform reports — it is never persisted.
struct FileStamp
{
    bool exists = false;
    std::uint64_t device = 0;
    std::uint64_t fileId = 0;
    std::int64_t modifiedTicks = 0;
    std::uintmax_t size = 0;

    bool operator==(const FileStamp& other) const = default;
};

FileStamp stampFile(const eacp::FilePath& path);
} // namespace detail

// A file held open for repeated appends — the journal's write handle. One
// open(), then each append() costs a single write syscall instead of an
// open/write/close round trip, which is most of the distance between
// appending a journal frame and a WAL insert in something like SQLite.
//
// Not atomic like writeFileAtomically — a crash mid-append can leave a
// partial tail, which is why only the binary journal writes this way: its
// reader validates each frame (length + checksum) and drops a torn tail, so
// a failed append is indistinguishable from one that never happened. Durable
// appends flush to disk after each write.
//
// The handle pins the inode it opened, not the path: close it before
// renaming over the file and whenever the file may have been replaced from
// outside. On Windows any open append handle — ours or another process's,
// FILE_SHARE_DELETE or not — makes a replace-by-rename of the file fail, so
// there the handle must not outlive a single append (see persistent).
class AppendFile
{
public:
    // Whether a handle may stay open between appends. POSIX only: on Windows
    // a held handle would block every other process's compaction.
#if defined(_WIN32)
    static constexpr bool persistent = false;
#else
    static constexpr bool persistent = true;
#endif

    AppendFile() = default;
    ~AppendFile();

    AppendFile(AppendFile&& other) noexcept;
    AppendFile& operator=(AppendFile&& other) noexcept;
    AppendFile(const AppendFile&) = delete;
    AppendFile& operator=(const AppendFile&) = delete;

    // Opens path for appending, creating the file if absent; an already-open
    // handle is closed first. Returns false and logs on failure.
    bool open(const eacp::FilePath& path);

    bool isOpen() const;
    void close();

    // Appends bytes at the current end of the file.
    bool append(std::string_view bytes, Durability durability);

    // The same identity stampFile(path) would compute, taken from the open
    // handle — one syscall, no path lookup. Empty when nothing is open.
    detail::FileStamp stamp() const;

private:
    // int fd on POSIX, HANDLE on Windows — held as an integer so this header
    // needs no <windows.h>.
    std::intptr_t handle = -1;
};
} // namespace emberstore
