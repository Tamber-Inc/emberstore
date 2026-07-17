// Unit tests for emberstore::Document<T> — the whole-value store that
// backs everything else. Drives the real on-disk path (temp files, rename,
// reload) because durable atomic persistence is the whole point: a fake
// filesystem would test nothing.

#include <emberstore/Emberstore.h>

#include <eacp/Network/IPC/Lock.h>

#include <NanoTest/NanoTest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace nano;
using namespace emberstore;

namespace
{
namespace fs = std::filesystem;

// A unique temp directory per test, removed when the test ends.
struct TempDir
{
    fs::path root;

    TempDir()
    {
        static std::atomic<int> counter {0};
        root = fs::temp_directory_path()
               / ("emberstore-storage-test-" + std::to_string(counter.fetch_add(1)));
        fs::remove_all(root);
        fs::create_directories(root);
    }

    ~TempDir() { fs::remove_all(root); }

    // A path INSIDE a not-yet-created subdirectory, to prove the store makes
    // parent directories itself.
    std::string file() const { return (root / "nested" / "data.json").string(); }
};

std::string readRaw(const std::string& path)
{
    auto in = std::ifstream {path, std::ios::binary};
    return std::string {std::istreambuf_iterator<char> {in}, {}};
}

void writeRaw(const std::string& path, const std::string& text)
{
    auto out = std::ofstream {path, std::ios::binary | std::ios::trunc};
    out << text;
}

struct Settings
{
    int version = 0;
    std::string note;
    std::vector<std::string> tags;

    MIRO_REFLECT(version, note, tags)
};
} // namespace

// --- Document -------------------------------------------------------------

auto tMissingFileReadsDefault =
    test("Document/a missing file reads as a default-constructed value") = []
{
    auto dir = TempDir {};
    auto doc = emberstore::Document<Settings> {dir.file()};

    auto s = doc.read();
    check(s.version == 0);
    check(s.note.empty());
    check(s.tags.empty());
    check(!fs::exists(dir.file())); // reading must not create the file
};

auto tWriteThenReadRoundTrips =
    test("Document/write then read returns the same value") = []
{
    auto dir = TempDir {};
    auto doc = emberstore::Document<Settings> {dir.file()};

    check(doc.write({7, "hi", {"a", "b"}}));

    auto s = doc.read();
    check(s.version == 7);
    check(s.note == "hi");
    check(s.tags == std::vector<std::string> {"a", "b"});
};

auto tWritePersistsAcrossInstances =
    test("Document/a written value survives being reopened from disk") = []
{
    auto dir = TempDir {};
    {
        auto doc = emberstore::Document<Settings> {dir.file()};
        doc.write({3, "kept", {"x"}});
    }
    // Fresh instance, cold cache — must load from the file on disk.
    auto reopened = emberstore::Document<Settings> {dir.file()};
    auto s = reopened.read();
    check(s.version == 3);
    check(s.note == "kept");
    check(s.tags == std::vector<std::string> {"x"});
};

auto tMutateEditsInPlace =
    test("Document/mutate edits the value and persists it") = []
{
    auto dir = TempDir {};
    auto doc = emberstore::Document<Settings> {dir.file()};
    doc.write({1, "", {}});

    check(doc.mutate(
        [](Settings& s)
        {
            s.version = 2;
            s.tags.push_back("t");
        }));

    auto reopened = emberstore::Document<Settings> {dir.file()};
    auto s = reopened.read();
    check(s.version == 2);
    check(s.tags == std::vector<std::string> {"t"});
};

auto tPicksUpExternalWrite =
    test("Document/sees a change written by another program") = []
{
    auto dir = TempDir {};
    auto doc = emberstore::Document<Settings> {dir.file()};

    doc.write({1, "mine", {}});
    check(doc.read().version == 1); // now cached

    // Another program (or your editor) replaces the file behind our back. The
    // cached parse must not be trusted past that.
    writeRaw(dir.file(), R"({"version":42,"note":"theirs","tags":["x"]})");

    auto s = doc.read();
    check(s.version == 42);
    check(s.note == "theirs");
};

// Stands in for another process: the same lock the Document guards its writes
// with, named by the document's path. flock/LockFileEx bind to the descriptor,
// not the process, so taking it here contends exactly as a second app would.
auto tWriteFailsWhileAnotherProcessHoldsTheLock =
    test("Document/write fails while another process holds the lock") = []
{
    auto dir = TempDir {};
    auto doc = emberstore::Document<Settings> {dir.file()};
    check(doc.write({1, "mine", {}}));

    auto other = eacp::IPC::Lock {dir.file()};
    {
        auto held = eacp::IPC::ScopedLock {other};
        check(held.isLocked());

        check(!doc.write({2, "blocked", {}}));
        check(doc.read().version == 1); // disk untouched
    } // lock free again

    check(doc.write({3, "after", {}}));
    check(doc.read().version == 3);
};

auto tMutateFailsWhileAnotherProcessHoldsTheLock =
    test("Document/mutate fails while another process holds the lock") = []
{
    auto dir = TempDir {};
    auto doc = emberstore::Document<Settings> {dir.file()};
    doc.write({1, "mine", {}});

    auto other = eacp::IPC::Lock {dir.file()};
    auto held = eacp::IPC::ScopedLock {other};
    check(held.isLocked());

    check(!doc.mutate([](Settings& s) { s.version = 99; }));
    check(doc.read().version == 1); // the read-modify-write never happened
};

// The point of holding the lock across the whole read-modify-write: whatever a
// sibling process published must be read back before `fn` edits it, or its
// write is silently overwritten.
auto tMutateSeesAnotherProcessesWrite =
    test("Document/mutate edits what another process last wrote") = []
{
    auto dir = TempDir {};
    auto doc = emberstore::Document<Settings> {dir.file()};
    doc.write({1, "mine", {}});
    check(doc.read().version == 1); // cached

    // Another process replaces the file while we hold a stale cache.
    writeRaw(dir.file(), R"({"version":7,"note":"theirs","tags":[]})");

    doc.mutate([](Settings& s) { s.version += 100; });

    auto s = doc.read();
    check(s.version == 107); // 7 + 100, not 1 + 100
    check(s.note == "theirs"); // their value survived our edit
};

auto tCorruptFileReadsDefault =
    test("Document/a corrupt file reads as default and never throws") = []
{
    auto dir = TempDir {};
    fs::create_directories(fs::path {dir.file()}.parent_path());
    writeRaw(dir.file(), "{ this is not valid json ]]");

    auto doc = emberstore::Document<Settings> {dir.file()};
    auto s = doc.read();
    check(s.version == 0);
    check(s.note.empty());
};

auto tWriteCreatesParentDirs =
    test("Document/write creates missing parent directories") = []
{
    auto dir = TempDir {};
    check(!fs::exists(fs::path {dir.file()}.parent_path()));

    auto doc = emberstore::Document<Settings> {dir.file()};
    check(doc.write({1, "n", {}}));
    check(fs::exists(dir.file()));
};

auto tWriteLeavesNoTempFile =
    test("Document/write leaves no .tmp sibling behind") = []
{
    auto dir = TempDir {};
    auto doc = emberstore::Document<Settings> {dir.file()};
    doc.write({1, "n", {}});

    check(fs::exists(dir.file()));
    check(!fs::exists(dir.file() + ".tmp"));
};

auto tWriteEmitsPrettyJson =
    test("Document/the file on disk is indented JSON with a trailing newline") = []
{
    auto dir = TempDir {};
    auto doc = emberstore::Document<Settings> {dir.file()};
    doc.write({5, "note", {"tag"}});

    auto raw = readRaw(dir.file());
    check(raw.find('\n') != std::string::npos); // indented => contains newlines
    check(!raw.empty() && raw.back() == '\n');
    check(raw.find("\"version\"") != std::string::npos);
};

auto tWriteOverwritesExisting =
    test("Document/a second write fully replaces the first") = []
{
    auto dir = TempDir {};
    auto doc = emberstore::Document<Settings> {dir.file()};
    doc.write({1, "first", {"a", "b", "c"}});
    doc.write({2, "second", {}});

    auto reopened = emberstore::Document<Settings> {dir.file()};
    auto s = reopened.read();
    check(s.version == 2);
    check(s.note == "second");
    check(s.tags.empty()); // the old three tags are gone, not merged
};

auto tAtomicDurabilityAlsoWorks =
    test("Document/Durability::Atomic writes and reloads correctly") = []
{
    auto dir = TempDir {};
    auto doc =
        emberstore::Document<Settings> {dir.file(), emberstore::Durability::Atomic};
    check(doc.write({9, "fast", {}}));

    auto reopened = emberstore::Document<Settings> {dir.file()};
    check(reopened.read().version == 9);
};

auto tWriteReplacesStaleTemp =
    test("Document/a leftover .tmp from a crash is overwritten, not appended") = []
{
    auto dir = TempDir {};
    fs::create_directories(fs::path {dir.file()}.parent_path());
    writeRaw(dir.file() + ".tmp", "garbage from a previous crashed write");

    auto doc = emberstore::Document<Settings> {dir.file()};
    check(doc.write({4, "clean", {}}));
    check(!fs::exists(dir.file() + ".tmp"));

    auto reopened = emberstore::Document<Settings> {dir.file()};
    check(reopened.read().version == 4);
};
