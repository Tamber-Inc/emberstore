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
// A cheap identity for a file's contents — last-write time plus size. Two
// absent files compare equal. Lets a cached parse be validated with a stat
// instead of re-reading and re-parsing the file.
struct FileStamp
{
    bool exists = false;
    std::int64_t modifiedTicks = 0;
    std::uintmax_t size = 0;

    bool operator==(const FileStamp& other) const = default;
};

FileStamp stampFile(const eacp::FilePath& path);
} // namespace detail
} // namespace emberstore
