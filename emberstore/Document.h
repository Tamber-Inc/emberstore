#pragma once

#include "AtomicFile.h"

#include <Miro/Reflect.h>
#include <eacp/Core/Threads/ThreadUtils.h>
#include "FileLock.h"

#include <chrono>
#include <optional>
#include <utility>

namespace emberstore
{
// A single Miro-reflected value persisted as one JSON file, read and written
// as a whole. Every write lands atomically (temp + rename, optionally fsync'd),
// so the file on disk is always a complete, valid T — the store can never
// brick the app by leaving a half-written value behind.
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
                      Durability durabilityToUse = Durability::Durable)
        : path(std::move(pathToUse))
        , durability(durabilityToUse)
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
        const auto held = ScopedLock {lock, lockTimeout};
        if (!held)
            return false;

        if (!persist(value))
        {
            cache.reset(); // disk is unchanged; don't trust what we have
            return false;
        }
        cache = value;
        stamp = detail::stampFile(path);
        return true;
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
        const auto held = ScopedLock {lock, lockTimeout};
        if (!held)
            return false;

        loaded();
        std::forward<Fn>(fn)(*cache);
        if (!persist(*cache))
        {
            cache.reset();
            return false;
        }
        stamp = detail::stampFile(path);
        return true;
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
            const auto text = readTextFile(path);
            cache = text ? Miro::createFromJSONString<T>(*text) : T {};
            stamp = current;
        }
        return *cache;
    }

    bool persist(const T& value)
    {
        return writeFileAtomically(
            path, Miro::toJSONString(value, 2) + "\n", durability);
    }

    // How long a write waits for another process before giving up. Bounded so a
    // stalled sibling app can't hang the message thread; the write just fails.
    static constexpr auto lockTimeout = std::chrono::milliseconds {250};

    eacp::FilePath path;
    Durability durability;
    std::optional<T> cache;
    detail::FileStamp stamp;

    // Locked beside the document, never the document itself: publishing by
    // rename replaces that file, so a lock on it would guard an orphaned inode.
    FileLock lock {eacp::FilePath {path.str() + ".lock"}};
};
} // namespace emberstore
