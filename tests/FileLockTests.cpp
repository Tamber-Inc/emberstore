// Tests for emberstore::FileLock.
//
// Two levels. The first uses a second FileLock in this process: flock() (macOS)
// and LockFileEx() (Windows) attach the lock to the open file description /
// handle rather than the process, so that contends the same way and covers the
// semantics cheaply.
//
// That alone proves nothing about crossing a real process boundary, though, and
// nothing about the OS releasing a dead holder's lock — which is the property
// the design leans on. So the tests at the bottom re-invoke this binary as a
// genuine second process (see tests/support/ContestFile.h).

#include <emberstore/FileLock.h>

#include "ContestFile.h"

#include <NanoTest/NanoTest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

using namespace nano;
using namespace emberstore;
using namespace std::chrono_literals;

namespace
{
namespace fs = std::filesystem;

struct TempDir
{
    fs::path root;

    TempDir()
    {
        static std::atomic<int> counter {0};
        root = fs::temp_directory_path()
               / ("emberstore-lock-test-" + std::to_string(counter.fetch_add(1)));
        fs::remove_all(root);
    }

    ~TempDir() { fs::remove_all(root); }

    // Inside a directory that doesn't exist yet, so the lock has to make it.
    std::string file() const { return (root / "nested" / ".lock").string(); }
};
} // namespace

auto tAcquireOnFreeLock = test("FileLock/acquires a lock nobody holds") = []
{
    auto dir = TempDir {};
    auto lock = emberstore::FileLock {dir.file()};

    check(!lock.isHeld());
    check(lock.tryAcquire());
    check(lock.isHeld());
};

auto tCreatesLockFileAndParents =
    test("FileLock/creates the lock file and its directory") = []
{
    auto dir = TempDir {};
    auto lock = emberstore::FileLock {dir.file()};
    check(lock.tryAcquire());

    check(fs::exists(dir.file()));
};

auto tSecondHolderIsExcluded =
    test("FileLock/a second holder cannot take a held lock") = []
{
    auto dir = TempDir {};
    auto first = emberstore::FileLock {dir.file()};
    check(first.tryAcquire());

    // A separate handle on the same file — what another process looks like.
    auto second = emberstore::FileLock {dir.file()};
    check(!second.tryAcquire());
    check(!second.isHeld());
};

auto tReleasePassesItOn = test("FileLock/releasing lets the next holder in") = []
{
    auto dir = TempDir {};
    auto first = emberstore::FileLock {dir.file()};
    auto second = emberstore::FileLock {dir.file()};

    check(first.tryAcquire());
    check(!second.tryAcquire());

    first.release();
    check(!first.isHeld());
    check(second.tryAcquire()); // now free
};

auto tDestructionReleases =
    test("FileLock/destroying a holder releases the lock") = []
{
    auto dir = TempDir {};
    auto waiting = emberstore::FileLock {dir.file()};

    {
        auto holder = emberstore::FileLock {dir.file()};
        check(holder.tryAcquire());
        check(!waiting.tryAcquire());
    } // holder destroyed without an explicit release

    check(waiting.tryAcquire());
};

auto tAcquireTimesOut =
    test("FileLock/acquire gives up when the lock stays held") = []
{
    auto dir = TempDir {};
    auto holder = emberstore::FileLock {dir.file()};
    check(holder.tryAcquire());

    auto waiting = emberstore::FileLock {dir.file()};
    const auto start = std::chrono::steady_clock::now();
    check(!waiting.acquire(80ms));
    const auto waited = std::chrono::steady_clock::now() - start;

    // It waited rather than failing instantly, and still returned.
    check(waited >= 60ms);
};

auto tAcquireSucceedsWithinTimeout =
    test("FileLock/acquire takes a free lock without waiting out the timeout") = []
{
    auto dir = TempDir {};
    auto lock = emberstore::FileLock {dir.file()};

    const auto start = std::chrono::steady_clock::now();
    check(lock.acquire(5s));
    const auto waited = std::chrono::steady_clock::now() - start;

    check(waited < 1s); // returned as soon as it had it
};

auto tReacquireIsIdempotent =
    test("FileLock/acquiring a lock you already hold is fine") = []
{
    auto dir = TempDir {};
    auto lock = emberstore::FileLock {dir.file()};

    check(lock.tryAcquire());
    check(lock.tryAcquire()); // still ours, not a deadlock
    check(lock.isHeld());

    lock.release();
    check(!lock.isHeld());
};

auto tReleaseWithoutAcquireIsHarmless =
    test("FileLock/releasing a lock you never took does nothing") = []
{
    auto dir = TempDir {};
    auto lock = emberstore::FileLock {dir.file()};

    lock.release(); // no-op, must not throw or wedge anything
    check(!lock.isHeld());
    check(lock.tryAcquire());
};

auto tIndependentPathsDoNotContend =
    test("FileLock/locks on different files are independent") = []
{
    auto dir = TempDir {};
    auto a = emberstore::FileLock {(dir.root / "a.lock").string()};
    auto b = emberstore::FileLock {(dir.root / "b.lock").string()};

    check(a.tryAcquire());
    check(b.tryAcquire()); // different resource, no contention
};

// --- Real processes -------------------------------------------------------
//
// Everything above uses a second FileLock in this process, which only proves
// the lock binds to the descriptor. These re-invoke the test binary so the
// contender is a genuine OS process — the thing the library actually claims to
// exclude. See tests/support/ContestFile.h.

auto tRealProcessCannotTakeALockWeHold =
    test("FileLock/another process cannot take a lock we hold") = []
{
    auto dir = TempDir {};
    auto ours = emberstore::FileLock {dir.file()};
    check(ours.tryAcquire());

    // A separate process tries for it and reports back.
    check(testing::runContender("try", dir.file()).find("busy")
          != std::string::npos);
};

auto tWeCannotTakeALockARealProcessHolds =
    test("FileLock/we cannot take a lock another process holds") = []
{
    auto dir = TempDir {};
    auto holder = testing::startLockHolder(dir.file(), 2000);
    check(holder != nullptr); // it announced that it has the lock

    auto ours = emberstore::FileLock {dir.file()};
    check(!ours.tryAcquire());

    holder->wait();
    check(ours.tryAcquire()); // released when it exited
};

auto tLockIsFreeOnceTheOtherProcessExits =
    test("FileLock/a lock is free again once the holding process exits") = []
{
    auto dir = TempDir {};

    // Runs to completion, taking and dropping the lock.
    check(testing::runContender("try", dir.file()).find("locked")
          != std::string::npos);

    auto ours = emberstore::FileLock {dir.file()};
    check(ours.tryAcquire());
};

// The property the whole design leans on: a holder that dies must not wedge the
// resource. Only the OS can clean that up — nothing in our code runs.
auto tCrashingProcessReleasesTheLock =
    test("FileLock/a process that dies holding the lock releases it") = []
{
    auto dir = TempDir {};

    check(testing::runContender("crash", dir.file()).find("locked")
          != std::string::npos); // it had the lock, then abort()ed

    auto ours = emberstore::FileLock {dir.file()};
    check(ours.acquire(2s)); // not wedged by the crash
};
