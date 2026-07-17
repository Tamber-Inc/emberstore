// Contention tests for a Database shared between programs: an eacp::IPC::Lock on
// the root is what serializes them, so these drive the two together — writers
// handing the root back and forth, a writer waiting out a busy holder, and a
// holder that "crashes" without releasing.
//
// The lock attaches to the open file description / handle (flock on macOS,
// LockFileEx on Windows), so a second Lock in this process contends exactly as a
// second process would. The Database itself is message-thread only (it asserts),
// so every Database call here stays on the test's main thread; only the lock is
// touched from a worker.

#include "ContestFile.h"
#include <emberstore/Emberstore.h>

#include <eacp/Core/Utils/Time.h>
#include <eacp/Network/IPC/Lock.h>

#include <NanoTest/NanoTest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

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
        root =
            fs::temp_directory_path()
            / ("emberstore-db-contention-" + std::to_string(counter.fetch_add(1)));
        fs::remove_all(root);
    }

    ~TempDir() { fs::remove_all(root); }

    std::string path() const { return root.string(); }
    std::string lockFile() const { return (root / ".lock").string(); }
};

struct Profile
{
    std::string name;
    int level = 0;

    MIRO_REFLECT(name, level)
};
} // namespace

auto tLockHandsTheDatabaseBetweenWriters =
    test("Contention/the root lock hands the database between two writers") = []
{
    auto dir = TempDir {};

    // Two writers fighting over the same root lock — A is the hub, B a
    // standalone app pointed at the same folder. Each holds the lock only for as
    // long as its guard is in scope, so dropping the guard hands the root over.
    auto aLock = eacp::IPC::Lock {dir.lockFile()};
    auto bLock = eacp::IPC::Lock {dir.lockFile()};
    auto aDb = emberstore::Database {dir.path()};
    auto bDb = emberstore::Database {dir.path()};

    {
        // A takes the root and writes. B, pointed at the same root, is locked
        // out while A holds it.
        auto aHeld = eacp::IPC::ScopedLock {aLock};
        check(aHeld.isLocked());
        aDb.document<Profile>("session").set({"hub", 1});

        auto bHeld = eacp::IPC::ScopedLock {bLock};
        check(!bHeld.isLocked());
    } // A hands the root over here

    {
        // B gets in, sees A's write, and replaces it.
        auto bHeld = eacp::IPC::ScopedLock {bLock};
        check(bHeld.isLocked());
        check(bDb.document<Profile>("session").get().name == "hub");
        bDb.document<Profile>("session").set({"standalone", 2});
    } // B hands it back

    // And back again — A picks up B's write, not its own stale value.
    auto aHeld = eacp::IPC::ScopedLock {aLock};
    check(aHeld.isLocked());
    check(aDb.document<Profile>("session").get().name == "standalone");
    check(aDb.document<Profile>("session").get().level == 2);
};

auto tAcquireOutlastsABusyHolder =
    test("Contention/a writer waits out a holder and finds the data intact") = []
{
    auto dir = TempDir {};
    emberstore::Database {dir.path()}.document<Profile>("owner").set({"first", 1});

    // Another program grabs the lock and sits on it for a while. Only the
    // lock lives on the worker thread — the Database is main-thread only.
    auto started = std::atomic<bool> {false};
    auto held = std::atomic<bool> {false};
    auto worker =
        std::thread {[&]
                     {
                         auto lock = eacp::IPC::Lock {dir.lockFile()};
                         auto guard = eacp::IPC::ScopedLock {lock};
                         held = guard.isLocked();
                         started = true;
                         if (guard.isLocked())
                             std::this_thread::sleep_for(100ms);
                     }}; // released by destruction, as if the holder exited

    while (!started)
        std::this_thread::yield();
    check(held);

    // Genuine contention: this polls against a live holder until it lets go.
    auto lock = eacp::IPC::Lock {dir.lockFile()};
    auto guard = eacp::IPC::ScopedLock {lock, eacp::Time::MS {5000}};
    check(guard.isLocked());
    worker.join();

    // The root is ours; the document written before the hand-off is intact.
    auto db = emberstore::Database {dir.path()};
    check(db.document<Profile>("owner").get().name == "first");
    check(db.document<Profile>("owner").set({"second", 2}));
};

auto tCrashedHolderDoesNotWedgeTheDatabase =
    test("Contention/a holder that dies without releasing frees the database") = []
{
    auto dir = TempDir {};

    {
        auto crashed = eacp::IPC::Lock {dir.lockFile()};
        auto guard = eacp::IPC::ScopedLock {crashed};
        check(guard.isLocked());
        emberstore::Database {dir.path()}.collection<Profile>("users").doc("a").set(
            {"Ann", 1});
    } // destroyed holding the lock — the OS releases it, as on a process crash

    // The next writer gets straight in and the data survived.
    auto lock = eacp::IPC::Lock {dir.lockFile()};
    auto guard = eacp::IPC::ScopedLock {lock};
    check(guard.isLocked());
    auto users = emberstore::Database {dir.path()}.collection<Profile>("users");
    check(users.doc("a").get()->name == "Ann");
    check(users.doc("b").set({"Bo", 2}));
    check(users.size() == 2);
};

// --- Real processes -------------------------------------------------------
//
// Everything above contends with a second Lock inside this process, which only
// exercises the lock's semantics. These re-invoke the test binary so the other
// side is a genuine OS process, and they go through Document's own per-file lock
// rather than a root lock taken by hand — i.e. the protection a caller gets for
// free.

auto tRealProcessBlocksAWrite = test(
    "Contention/a write fails while a real process holds the document lock") = []
{
    auto dir = TempDir {};
    auto db = emberstore::Database {dir.path()};
    auto doc = db.document<Profile>("session");
    check(doc.write({"mine", 1}));

    // The name Document locks under is its own file path — hold that and the
    // real process contends exactly as another writer would.
    const auto lockName = doc.filePath().str();
    auto holder = testing::startLockHolder(lockName, 2000);
    check(holder != nullptr); // it announced that it holds the lock

    check(!doc.write({"blocked", 2}));
    check(doc.read().level == 1); // disk untouched

    holder->wait();
    check(doc.write({"after", 3})); // free again once it exited
};

// The read-modify-write window is the one that silently loses data, so prove a
// real process is kept out of the whole cycle rather than just the write.
auto tRealProcessIsExcludedForTheWholeMutate =
    test("Contention/mutate keeps a real process out for the whole cycle") = []
{
    auto dir = TempDir {};
    auto db = emberstore::Database {dir.path()};
    auto doc = db.document<Profile>("session");
    doc.write({"mine", 1});

    const auto lockName = doc.filePath().str();
    auto sawBusy = false;
    check(doc.mutate(
        [&](Profile& profile)
        {
            // A real process asks for the lock while we are mid-edit.
            sawBusy = testing::runContender("try", lockName).find("busy")
                      != std::string::npos;
            profile.level = 2;
        }));

    check(sawBusy);
    check(doc.read().level == 2);
};
