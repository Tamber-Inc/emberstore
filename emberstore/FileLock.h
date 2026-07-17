#pragma once

#include <eacp/Core/Utils/FilePath.h>

#include <chrono>
#include <memory>
#include <thread>

namespace emberstore
{
// An exclusive lock held across processes, backed by a lock file: flock() on
// macOS, LockFileEx() on Windows. Two processes (say hub and a standalone app)
// pointed at the same lock file can hand a resource between them instead of
// racing each other's writes.
//
//   auto lock = FileLock {databaseDir / ".lock"};
//   if (lock.acquire(std::chrono::milliseconds {250}))
//       ... // exclusive until release() or destruction
//
// The lock is released when this object is destroyed — and, crucially, by the
// OS if the process dies holding it, so a crash cannot wedge the resource with
// a stale lock the way a hand-rolled "is-running" marker file would.
//
// Lock a file that stands *beside* the resource, not the resource itself: a
// store that publishes by rename replaces its file, so a lock taken on it ends
// up held against an orphaned inode while everyone else locks the new one, and
// silently excludes nobody. The lock file is created on first acquire and left
// behind; only the lock on it matters, never its contents.
//
// Advisory, not mandatory: it only excludes other holders of the same lock
// file, and does nothing to stop a program that ignores it from writing.
// Not reentrant across FileLock objects in one process — a second FileLock on
// the same path contends with the first, exactly as another process would.
class FileLock
{
public:
    // Records the path. Touches nothing on disk until the first acquire, so
    // merely holding a FileLock never creates files or directories.
    explicit FileLock(const eacp::FilePath& path);
    ~FileLock();

    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    FileLock(FileLock&&) noexcept;
    FileLock& operator=(FileLock&&) noexcept;

    // Takes the lock if it is free right now, creating the lock file if needed.
    // False if another holder has it, or the file couldn't be opened.
    bool tryAcquire();

    // Takes the lock, giving up after `timeout`. Polls rather than blocking
    // outright, so a caller on the message thread can bound the stall.
    template <typename Rep, typename Period>
    bool acquire(std::chrono::duration<Rep, Period> timeout)
    {
        constexpr auto poll = std::chrono::milliseconds {5};
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;)
        {
            if (tryAcquire())
                return true;
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(poll);
        }
    }

    void release();
    bool isHeld() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Holds a FileLock for a scope. Check it before touching the resource — an
// unheld guard means the lock timed out and someone else still has it.
//
//   const auto held = ScopedLock {lock, 250ms};
//   if (!held)
//       return false;
class ScopedLock
{
public:
    template <typename Rep, typename Period>
    ScopedLock(FileLock& lockToUse, std::chrono::duration<Rep, Period> timeout)
        : lock(&lockToUse)
        , acquired(lockToUse.acquire(timeout))
    {
    }

    ~ScopedLock()
    {
        if (acquired)
            lock->release();
    }

    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;

    explicit operator bool() const { return acquired; }

private:
    FileLock* lock;
    bool acquired;
};
} // namespace emberstore
