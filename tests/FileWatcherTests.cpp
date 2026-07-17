// Tests for emberstore::FileWatcher. The native watcher delivers on the
// main thread's run loop, so these pump it via eacp's runEventLoopUntil — no
// platform code here; CMake decides which watcher backend is compiled in.

#include <emberstore/FileWatcher.h>

#include <eacp/Core/Threads/EventLoop.h>

#include <NanoTest/NanoTest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

using namespace nano;
using namespace emberstore;

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
             / ("emberstore-watch-test-" + std::to_string(counter.fetch_add(1)));
        fs::remove_all(root);
        fs::create_directories(root);
    }

    ~TempDir() { fs::remove_all(root); }

    std::string file(const char* name) const { return (root / name).string(); }
};

void writeFile(const std::string& path, const std::string& text)
{
    auto out = std::ofstream {path, std::ios::binary | std::ios::trunc};
    out << text;
}

// Let the watcher establish and drain any events from files that already
// existed when it started, so a test only counts changes it makes itself.
void settle(std::atomic<int>& changes)
{
    eacp::Threads::runEventLoopFor(eacp::Time::MS {500});
    changes.store(0);
}

bool waitForChange(const std::atomic<int>& changes)
{
    return eacp::Threads::runEventLoopUntil([&] { return changes.load() > 0; },
                                            eacp::Time::MS {5000});
}
} // namespace

auto tFiresOnModify = test("FileWatcher/fires when the watched file changes") = []
{
    auto dir = TempDir {};
    const auto path = dir.file("watched.json");
    writeFile(path, "one");

    auto changes = std::atomic<int> {0};
    auto watcher =
        emberstore::FileWatcher {eacp::FilePath {path}, [&] { ++changes; }};
    settle(changes);

    writeFile(path, "two");
    check(waitForChange(changes));
};

auto tFiresOnCreate =
    test("FileWatcher/fires when the watched file is created") = []
{
    auto dir = TempDir {};
    const auto path = dir.file("later.json"); // does not exist yet

    auto changes = std::atomic<int> {0};
    auto watcher =
        emberstore::FileWatcher {eacp::FilePath {path}, [&] { ++changes; }};
    settle(changes);

    writeFile(path, "hello");
    check(waitForChange(changes));
};

auto tIgnoresSiblingFiles =
    test("FileWatcher/does not fire for a different file in the same dir") = []
{
    auto dir = TempDir {};
    const auto watched = dir.file("watched.json");
    writeFile(watched, "one");

    auto changes = std::atomic<int> {0};
    auto watcher =
        emberstore::FileWatcher {eacp::FilePath {watched}, [&] { ++changes; }};
    settle(changes);

    writeFile(dir.file("other.json"), "unrelated"); // sibling, not watched
    eacp::Threads::runEventLoopFor(eacp::Time::MS {1000}); // give it a chance
    check(changes.load() == 0);
};

auto tStopsAfterDestruction =
    test("FileWatcher/stops delivering once destroyed") = []
{
    auto dir = TempDir {};
    const auto path = dir.file("watched.json");
    writeFile(path, "one");

    auto changes = std::atomic<int> {0};
    {
        auto watcher =
            emberstore::FileWatcher {eacp::FilePath {path}, [&] { ++changes; }};
        settle(changes);
    } // watcher destroyed here

    writeFile(path, "two");
    eacp::Threads::runEventLoopFor(eacp::Time::MS {1000});
    check(changes.load() == 0);
};
