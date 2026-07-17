// Tests for emberstore::LiveDocument<T>'s external-change path: a write
// by someone who is not us (another process, an editor) must reach the file
// watcher and republish through the Miro event — while our own writes and
// no-op touches must not echo a second publish. Pumps the real event loop,
// like the FileWatcher tests.

#include <emberstore/Live.h>
#include <emberstore/Emberstore.h>

#include <ea_data_structures/Pointers/Broadcaster.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Network/IPC/Lock.h>

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
               / ("emberstore-live-test-" + std::to_string(counter.fetch_add(1)));
        fs::remove_all(root);
        fs::create_directories(root);
    }

    ~TempDir() { fs::remove_all(root); }

    std::string file() const { return (root / "pet.json").string(); }
};

struct Pet
{
    std::string name;
    int age = 0;

    MIRO_REFLECT(name, age)
};

void writeRaw(const std::string& path, const std::string& text)
{
    auto out = std::ofstream {path, std::ios::binary | std::ios::trunc};
    out << text;
}

// One live store with a listener counting every publish of its event.
struct LiveFixture
{
    explicit LiveFixture(const TempDir& dir)
        : live(emberstore::Document<Pet> {dir.file()}, event)
    {
    }

    // Let the watcher establish and drain events from our own setup writes,
    // so a test only counts the publishes it provokes itself.
    void settle()
    {
        eacp::Threads::runEventLoopFor(eacp::Time::MS {500});
        publishes.store(0);
    }

    bool waitForPublish()
    {
        return eacp::Threads::runEventLoopUntil([&] { return publishes.load() > 0; },
                                                eacp::Time::MS {5000});
    }

    Miro::Event<Pet> event;
    emberstore::LiveDocument<Pet> live;
    std::atomic<int> publishes {0};
    EA::Listener listener {event.broadcaster(),
                           [this] { ++publishes; },
                           EA::Listener::Modes::TriggerOnEvent};
};
} // namespace

auto tExternalChangeRepublishes =
    test("LiveDocument/republishes when the file changes externally") = []
{
    auto dir = TempDir {};
    auto fx = LiveFixture {dir};

    fx.live.set({"Rex", 3});
    fx.live.watchForExternalChanges();
    fx.settle();

    // Someone who is not us: bypass the store and rewrite the file raw.
    writeRaw(dir.file(), Miro::toJSONString(Pet {"Fido", 5}, 2) + "\n");

    check(fx.waitForPublish());
    check(fx.event.snapshot().name == "Fido");
    check(fx.event.snapshot().age == 5);
};

auto tExternalCreateRepublishes =
    test("LiveDocument/republishes when the file is first created externally") = []
{
    auto dir = TempDir {};
    auto fx = LiveFixture {dir};

    fx.live.watchForExternalChanges(); // file does not exist yet
    fx.settle();

    writeRaw(dir.file(), Miro::toJSONString(Pet {"Luna", 1}, 2) + "\n");

    check(fx.waitForPublish());
    check(fx.event.snapshot().name == "Luna");
};

auto tOwnWriteDoesNotEcho =
    test("LiveDocument/its own set publishes once, with no watcher echo") = []
{
    auto dir = TempDir {};
    auto fx = LiveFixture {dir};

    fx.live.set({"Rex", 3});
    fx.live.watchForExternalChanges();
    fx.settle();

    fx.live.set({"Rex", 4}); // publishes synchronously...
    check(fx.publishes.load() == 1);

    // ...and the watcher event for that same write must be filtered out.
    eacp::Threads::runEventLoopFor(eacp::Time::MS {1000});
    check(fx.publishes.load() == 1);
};

auto tNoOpTouchDoesNotRepublish =
    test("LiveDocument/an external rewrite of identical content is silent") = []
{
    auto dir = TempDir {};
    auto fx = LiveFixture {dir};

    fx.live.set({"Rex", 3});
    fx.live.watchForExternalChanges();
    fx.settle();

    writeRaw(dir.file(), Miro::toJSONString(Pet {"Rex", 3}, 2) + "\n");

    eacp::Threads::runEventLoopFor(eacp::Time::MS {1000});
    check(fx.publishes.load() == 0);
};

// --- Inter-process lock ---------------------------------------------------

// Why update() hands the whole cycle to Document::mutate instead of composing
// get() + set(): the lock has to cover the read *and* the write, or a sibling
// process can slip a change into the gap and have it edited over.
auto tUpdateHoldsTheLockAcrossReadModifyWrite =
    test("LiveDocument/update holds the lock for the whole read-modify-write") = []
{
    auto dir = TempDir {};
    auto fx = LiveFixture {dir};
    fx.live.set({"Rex", 3});

    auto lockedWhileEditing = false;
    fx.live.update(
        [&](Pet& pet)
        {
            // Another process could only get in here if the lock had been
            // dropped between reading and writing.
            auto other = eacp::IPC::Lock {dir.file()};
            auto guard = eacp::IPC::ScopedLock {other};
            lockedWhileEditing = !guard.isLocked();
            pet.age = 4;
        });

    check(lockedWhileEditing);
    check(fx.live.get().age == 4);
};

auto tUpdateEditsWhatAnotherProcessWrote =
    test("LiveDocument/update edits what another process last wrote") = []
{
    auto dir = TempDir {};
    auto fx = LiveFixture {dir};
    fx.live.set({"Rex", 3});

    writeRaw(dir.file(), R"({"name":"Fido","age":10})");

    check(fx.live.update([](Pet& pet) { pet.age += 1; }));

    const auto pet = fx.live.get();
    check(pet.age == 11); // 10 + 1, not 3 + 1
    check(pet.name == "Fido"); // their write survived our edit
};

auto tBlockedWriteDoesNotPublish =
    test("LiveDocument/a write blocked by another process publishes nothing") = []
{
    auto dir = TempDir {};
    auto fx = LiveFixture {dir};
    fx.live.set({"Rex", 3});
    fx.publishes.store(0);

    auto other = eacp::IPC::Lock {dir.file()};
    auto guard = eacp::IPC::ScopedLock {other};
    check(guard.isLocked());

    check(!fx.live.set({"Blocked", 9}));
    check(!fx.live.update([](Pet& pet) { pet.age = 99; }));

    // Subscribers must never see a value that isn't on disk.
    check(fx.publishes.load() == 0);
    check(fx.event.snapshot().age == 3);
};
