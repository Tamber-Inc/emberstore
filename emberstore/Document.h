#pragma once

#include "AtomicFile.h"

#include <Miro/Reflect.h>
#include <eacp/Core/Threads/ThreadUtils.h>
#include <eacp/Core/Utils/Time.h>
#include <eacp/Network/IPC/Lock.h>

#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace emberstore
{
namespace detail
{
template <typename Store>
class TransactionalStore;

// Whether a store writes its file on every change, as Database's stores do, or
// keeps the changes in memory until a TransactionalDatabase commits them.
enum class WriteMode
{
    CommitEachChange,
    StageUntilCommit,
};
} // namespace detail

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
        return visibleValue();
    }

    T get() { return read(); }
    bool set(const T& value) { return write(value); }

    // The current value without copying it. Valid until the next write through
    // this Document (or, with staged changes, the next read) — read it, don't
    // hold it. Lets callers scan a large value without cloning it.
    const T& peek()
    {
        eacp::Threads::assertMainThread();
        return visibleValue();
    }

    // Replaces the whole value and persists it. Returns false (and logs) on IO
    // failure, or if another process holds the lock for longer than
    // `lockTimeout` — in both cases the target file is left untouched. In a
    // TransactionalDatabase the value is staged instead and this returns true.
    bool write(const T& value)
    {
        eacp::Threads::assertMainThread();
        if (writeMode == detail::WriteMode::StageUntilCommit)
        {
            stagedChanges.emplace_back(std::in_place_type<T>, value);
            return true;
        }
        return commitChange(StartingValue::Replaced,
                            [&](std::optional<T>& current) { current = value; });
    }

    // Reads, hands the value to `fn` to edit in place, then persists it. The
    // edit happens on the cached value, so it costs no extra copy.
    //
    // The lock spans the whole read-modify-write, not just the write: that is
    // the window where a second process's change would otherwise be read,
    // edited over, and published back — silently losing it. Loading inside the
    // lock also means the stamp check picks up whatever that process wrote, so
    // `fn` edits current data rather than a stale snapshot.
    //
    // In a TransactionalDatabase `fn` itself is staged: commit runs it again,
    // under the lock, on whatever the file then holds, and every read before
    // that runs it on a copy. Keep it to editing the value.
    template <typename Fn>
    bool mutate(Fn&& fn)
    {
        eacp::Threads::assertMainThread();
        if (writeMode == detail::WriteMode::StageUntilCommit)
        {
            stagedChanges.emplace_back(std::in_place_type<Edit>,
                                       std::forward<Fn>(fn));
            return true;
        }
        return commitChange(StartingValue::Stored,
                            [&](std::optional<T>& current)
                            { std::forward<Fn>(fn)(*current); });
    }

    const eacp::FilePath& filePath() const { return path; }

private:
    template <typename Store>
    friend class detail::TransactionalStore;

    using Edit = std::function<void(T&)>;
    using StagedChange = std::variant<T, Edit>;

    enum class StartingValue
    {
        Stored,
        Replaced,
    };

    // The one write path. Takes the lock, loads the file when the change starts
    // from it, applies the change to the cache and writes it once.
    template <typename Change>
    bool commitChange(StartingValue startingValue, Change&& change)
    {
        auto* const guard = lockHandle();
        if (guard == nullptr)
            return false;
        const auto held = eacp::IPC::ScopedLock {*guard, lockTimeout};
        if (!held)
            return false;

        if (startingValue == StartingValue::Stored)
            loaded();
        std::forward<Change>(change)(cache);
        if (!persist(*cache))
        {
            cache.reset(); // disk is unchanged; don't trust what we have
            return false;
        }
        stamp = detail::stampFile(path);
        return true;
    }

    bool commitStagedChanges()
    {
        if (stagedChanges.empty())
            return true;

        const auto committed = commitChange(startingValueOfStagedChanges(),
                                            [this](std::optional<T>& current)
                                            { applyStagedChanges(current); });
        if (committed)
            discardStagedChanges();
        return committed;
    }

    void discardStagedChanges()
    {
        stagedChanges.clear();
        stagedView.reset();
    }

    bool hasStagedChanges() const { return !stagedChanges.empty(); }

    void stageWritesUntilCommit()
    {
        writeMode = detail::WriteMode::StageUntilCommit;
    }

    StartingValue startingValueOfStagedChanges() const
    {
        if (std::holds_alternative<T>(stagedChanges.front()))
            return StartingValue::Replaced;
        return StartingValue::Stored;
    }

    void applyStagedChanges(std::optional<T>& value) const
    {
        for (const auto& change: stagedChanges)
        {
            if (std::holds_alternative<T>(change))
                value = std::get<T>(change);
            else
                std::get<Edit>(change)(*value);
        }
    }

    const T& visibleValue()
    {
        if (stagedChanges.empty())
            return loaded();

        stagedView.reset();
        if (startingValueOfStagedChanges() == StartingValue::Stored)
            stagedView = loaded();
        applyStagedChanges(stagedView);
        return *stagedView;
    }

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
    std::optional<T> cache;
    detail::FileStamp stamp;

    // One interprocess lock per document, named by the document's path — eacp
    // keys it to a lock file of its own, so nothing lands beside the document
    // for a publish-by-rename to orphan. Held via unique_ptr so a Document stays
    // movable (an eacp::IPC::Lock is not) and constructing one still touches
    // nothing on disk until the first write actually takes the lock.
    std::unique_ptr<eacp::IPC::Lock> lock;

    detail::WriteMode writeMode = detail::WriteMode::CommitEachChange;
    std::vector<StagedChange> stagedChanges;
    std::optional<T> stagedView;
};
} // namespace emberstore
