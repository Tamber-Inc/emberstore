#pragma once

#include "AtomicFile.h"

#include <Calder/Format.h>
#include <Miro/Reflect.h>
#include <eacp/Core/Threads/ThreadUtils.h>
#include <eacp/Core/Utils/Time.h>
#include <eacp/Network/IPC/Lock.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace emberstore
{
namespace detail
{
inline std::span<const std::uint8_t> asBytes(std::string_view text)
{
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

inline std::string_view asText(const std::vector<std::uint8_t>& bytes)
{
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// A stored file announces Calder's format with its "CLD1" (document)
// or "CLDJ" (journal) magic; anything else is treated as JSON. This is
// what lets reads ignore the configured Format entirely.
inline bool looksBinary(std::string_view bytes)
{
    return Calder::Detail::isEncoded(asBytes(bytes));
}

// What Document knows about the file it last read or wrote, when that file
// is a binary journal: whether appending is possible at all, how big the
// base and the appended tail are (the compaction trigger), and whether the
// tail ended torn — in which case the next write compacts, healing the file.
// keyTable is the base's, which every appended frame's key ids index.
struct JournalState
{
    bool active = false;
    bool damaged = false;
    std::size_t baseBytes = 0;
    std::size_t tailBytes = 0;
    Calder::Detail::InternedKeyTable keyTable;
};

inline JournalState journalStateOf(std::string_view bytes)
{
    const auto data = asBytes(bytes);

    if (!Calder::Detail::isJournal(data))
        return {};

    const auto info = Calder::Detail::inspectJournal(data);

    if (!info.valid)
        return {};

    return {true,
            info.truncated,
            info.baseSize,
            info.frameBytes,
            Calder::Detail::readJournalKeyTable(data)};
}
} // namespace detail

// A single Miro-reflected value persisted as one file — JSON or Miro's binary
// wire format (see Format). Whole-value writes land atomically (temp + rename,
// optionally fsync'd), so the file is never a half-written T. Binary
// single-entry writes append a journal frame instead; those rely on frame
// validation (length + checksum) and torn-tail recovery on load: a crash
// mid-append loses at most that frame, and the next write heals the file.
//
//   auto settings = Document<AppSettings> {path};
//   auto current  = settings.read();
//   settings.mutate([](AppSettings& s) { s.volume = 0.8; });
//
// The parsed value is cached in memory, so repeat reads cost a stat rather than
// a re-read and re-parse. The cache is validated against the file's last-write
// time and size on every access, so a change from outside — another program,
// your editor — is still picked up; our own writes refresh it directly.
//
// Message-thread only. It holds no locks — the single-thread rule is what makes
// the cache safe — and every method asserts it. A missing or corrupt file loads
// as a default-constructed T (logged, never thrown).
template <typename T>
class Document
{
public:
    explicit Document(eacp::FilePath pathToUse,
                      Durability durabilityToUse = Durability::Durable,
                      Format formatToUse = Format::Json)
        : path(std::move(pathToUse))
        , durability(durabilityToUse)
        , format(formatToUse)
    {
    }

    Document(eacp::FilePath pathToUse,
             Format formatToUse,
             Durability durabilityToUse = Durability::Durable)
        : Document(std::move(pathToUse), durabilityToUse, formatToUse)
    {
    }

    // A snapshot copy of the current value. `get` is the Firebase-style alias.
    T read()
    {
        eacp::Threads::assertMainThread();
        return loaded();
    }

    T get() { return read(); }
    bool set(const T& value) { return write(value); }

    // The current value without copying it. Valid until the next write through
    // this Document — read it, don't hold it. Lets callers scan a large value
    // without cloning it.
    const T& peek()
    {
        eacp::Threads::assertMainThread();
        return loaded();
    }

    // Replaces the whole value and persists it. Returns false (and logs) on IO
    // failure, or if another process holds the lock for longer than
    // `lockTimeout` — in both cases the target file is left untouched.
    bool write(const T& value)
    {
        eacp::Threads::assertMainThread();
        auto* const guard = lockHandle();
        if (guard == nullptr)
            return false;
        const auto held = eacp::IPC::ScopedLock {*guard, lockTimeout};
        if (!held)
            return false;

        if (!persist(value))
        {
            cache.reset(); // disk is unchanged; don't trust what we have
            return false;
        }
        cache = value;
        journal = {}; // persist() writes a plain document, not a journal
        stamp = detail::stampFile(path);
        return true;
    }

    // Sets one entry of a map-shaped document, appending a journal frame
    // instead of rewriting the file (see Format::Binary in AtomicFile.h);
    // under Format::Json it falls back to the whole-file write. Only
    // compiles for T with map semantics — Collection is the caller.
    template <typename V>
    bool set(const std::string& key, const V& value)
    {
        eacp::Threads::assertMainThread();
        auto* const guard = lockHandle();
        if (guard == nullptr)
            return false;
        const auto held = eacp::IPC::ScopedLock {*guard, lockTimeout};
        if (!held)
            return false;

        loaded();
        (*cache)[key] = value;

        if (format != Format::Binary)
            return persistLocked();

        // A value naming a field the journal's base lacks gets no frame: a
        // fresh base that has the name keeps every frame readable on its own.
        auto frame = Calder::Detail::makeJournalSetFrame(key, value, journal.keyTable);

        if (!frame.hasFrame())
            return compact();

        return appendOrCompact(frame.frame);
    }

    // Removes one entry of a map-shaped document by appending a tombstone
    // frame. Removing an absent key is a successful no-op — nothing needs
    // to reach the disk for the file to already mean "not there".
    bool remove(const std::string& key)
    {
        eacp::Threads::assertMainThread();
        auto* const guard = lockHandle();
        if (guard == nullptr)
            return false;
        const auto held = eacp::IPC::ScopedLock {*guard, lockTimeout};
        if (!held)
            return false;

        loaded();
        if (cache->find(key) == cache->end())
            return true;
        cache->erase(key);

        if (format != Format::Binary)
            return persistLocked();

        return appendOrCompact(Calder::Detail::makeJournalRemoveFrame(key));
    }

    // Reads, hands the value to `fn` to edit in place, then persists it. The
    // edit happens on the cached value, so it costs no extra copy.
    //
    // The lock spans the whole read-modify-write, not just the write: that is
    // the window where a second process's change would otherwise be read,
    // edited over, and published back — silently losing it. Loading inside the
    // lock also means the stamp check picks up whatever that process wrote, so
    // `fn` edits current data rather than a stale snapshot.
    template <typename Fn>
    bool mutate(Fn&& fn)
    {
        eacp::Threads::assertMainThread();
        auto* const guard = lockHandle();
        if (guard == nullptr)
            return false;
        const auto held = eacp::IPC::ScopedLock {*guard, lockTimeout};
        if (!held)
            return false;

        loaded();
        std::forward<Fn>(fn)(*cache);
        return persistLocked();
    }

    const eacp::FilePath& filePath() const { return path; }

private:
    // Returns the cached value, re-reading only when the file on disk no longer
    // matches the stamp we cached it against.
    const T& loaded()
    {
        const auto current = detail::stampFile(path);
        if (!cache || !(current == stamp))
        {
            // The file changed under us (or was never read): the append
            // handle may pin a replaced inode, so let it go with the cache.
            appender.close();
            const auto text = readTextFile(path);
            journal = text ? detail::journalStateOf(*text) : detail::JournalState {};
            cache = text ? decode(*text) : T {};
            stamp = current;
        }
        return *cache;
    }

    // Persists the cached value whole (called with the lock held). Any
    // whole-value write leaves a plain document behind, so the journal
    // bookkeeping resets.
    bool persistLocked()
    {
        if (!persist(*cache))
        {
            cache.reset();
            journal = {};
            return false;
        }
        journal = {};
        stamp = detail::stampFile(path);
        return true;
    }

    // The binary write path for single-entry changes: append the frame when
    // the file is already a healthy journal with a modest tail, otherwise
    // rewrite the whole current value as a fresh journal base (one atomic
    // write — this is both the "convert whatever was there" step and the
    // compaction that stops a long-lived journal growing without bound).
    bool appendOrCompact(const std::vector<std::uint8_t>& frame)
    {
        if (journal.active && !journal.damaged
            && journal.tailBytes + frame.size() <= compactionLimit())
        {
            if (!appender.isOpen())
            {
                if (!appender.open(path))
                    return failAppend();

                // The file vanished or was replaced between loaded() and this
                // open: appending would write a bare frame to a headerless
                // file. Rewrite the whole value instead.
                if (!(appender.stamp() == stamp))
                {
                    appender.close();
                    return compact();
                }
            }

            if (!appender.append(detail::asText(frame), durability))
                return failAppend();

            journal.tailBytes += frame.size();
            if (AppendFile::persistent)
                stamp = appender.stamp();
            else
            {
                // Stamped after the close, in case closing touches the
                // last-write time — otherwise every set would reload.
                appender.close();
                stamp = detail::stampFile(path);
            }
            return true;
        }

        return compact();
    }

    bool failAppend()
    {
        appender.close();
        cache.reset();
        journal = {};
        return false;
    }

    // Rewrites the whole current value as a fresh journal base.
    bool compact()
    {
        appender.close(); // about to rename over the file
        const auto bytes = Calder::Detail::makeJournal(Calder::toBinary(*cache));
        if (!writeFileAtomically(path, detail::asText(bytes), durability))
        {
            cache.reset();
            journal = {};
            return false;
        }
        journal = {.active = true,
                   .damaged = false,
                   .baseBytes = bytes.size() - Calder::Detail::headerSize,
                   .tailBytes = 0,
                   .keyTable = Calder::Detail::readJournalKeyTable(bytes)};
        stamp = detail::stampFile(path);
        return true;
    }

    // Appended frames may grow the file to roughly twice its compacted size
    // before a write folds them into a fresh base; the floor stops tiny
    // stores compacting on every write.
    std::size_t compactionLimit() const
    {
        return std::max<std::size_t>(journal.baseBytes, std::size_t {4096});
    }

    // Decodes by what the file actually contains, not by `format` — a
    // Document pointed at a file of the other encoding still reads it, and
    // its next write converts it.
    static T decode(const std::string& bytes)
    {
        if (detail::looksBinary(bytes))
            return Calder::createFromBinary<T>(bytes).value_or(T {});

        return Miro::createFromJSONString<T>(bytes);
    }

    bool persist(const T& value)
    {
        appender.close(); // every persist renames over the file

        if (format == Format::Binary)
        {
            const auto bytes = Calder::toBinary(value);
            return writeFileAtomically(
                path,
                std::string_view {reinterpret_cast<const char*>(bytes.data()),
                                  bytes.size()},
                durability);
        }

        return writeFileAtomically(
            path, Miro::toJSONString(value, 2) + "\n", durability);
    }

    // The interprocess lock, created on first use. Null only if eacp cannot back
    // the name with a file at all, which a caller sees as a failed acquire — the
    // same outcome as another process holding it, never a thrown exception.
    eacp::IPC::Lock* lockHandle()
    {
        if (!lock)
        {
            try
            {
                lock = std::make_unique<eacp::IPC::Lock>(path.str());
            }
            catch (const eacp::IPC::Error&)
            {
                return nullptr;
            }
        }
        return lock.get();
    }

    // How long a write waits for another process before giving up. Bounded so a
    // stalled sibling app can't hang the message thread; the write just fails.
    static constexpr auto lockTimeout = eacp::Time::MS {250};

    eacp::FilePath path;
    Durability durability;
    Format format;
    std::optional<T> cache;
    detail::FileStamp stamp;
    detail::JournalState journal;

    // The journal's write handle, opened lazily by the first append and, on
    // POSIX, kept open across appends — one write syscall per frame. Closed
    // whenever the file is renamed over or reloaded, and after every append
    // on Windows (see AppendFile::persistent).
    AppendFile appender;

    // One interprocess lock per document, named by the document's path — eacp
    // keys it to a lock file of its own, so nothing lands beside the document
    // for a publish-by-rename to orphan. Held via unique_ptr so a Document stays
    // movable (an eacp::IPC::Lock is not) and constructing one still touches
    // nothing on disk until the first write actually takes the lock.
    std::unique_ptr<eacp::IPC::Lock> lock;
};
} // namespace emberstore
