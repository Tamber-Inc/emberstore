// Tests for Format::Binary — the packed Miro wire format as a storage
// encoding — and for moving a store between formats: reads sniff the actual
// file content, Database adopts the other format's file on open, and each
// store converts on its next write. Includes a size/speed comparison against
// the JSON encoding, printed for inspection; only the invariants are asserted.

#include <emberstore/Emberstore.h>

#include <NanoTest/NanoTest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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
               / ("emberstore-format-test-" + std::to_string(counter.fetch_add(1)));
        fs::remove_all(root);
        fs::create_directories(root);
    }

    ~TempDir() { fs::remove_all(root); }

    std::string file(const char* name) const { return (root / name).string(); }
};

std::string readRaw(const std::string& path)
{
    auto in = std::ifstream {path, std::ios::binary};
    return std::string {std::istreambuf_iterator<char> {in}, {}};
}

// Either binary magic: "MIB1" (plain document) or "MIBJ" (journal).
bool startsWithMagic(const std::string& bytes)
{
    return bytes.size() >= 4
           && (bytes.compare(0, 4, "MIB1") == 0 || bytes.compare(0, 4, "MIBJ") == 0);
}

struct Track
{
    std::string title;
    double duration = 0.0;
    std::vector<float> embedding;

    MIRO_REFLECT(title, duration, embedding)
};

struct Playlist
{
    std::string name;
    std::vector<Track> tracks;

    MIRO_REFLECT(name, tracks)
};

Playlist makePlaylist(int trackCount)
{
    auto playlist = Playlist {};
    playlist.name = "bench";

    for (auto i = 0; i < trackCount; ++i)
    {
        auto track = Track {};
        track.title = "Track " + std::to_string(i);
        track.duration = 180.0 + i * 0.377;

        for (auto d = 0; d < 8; ++d)
            track.embedding.push_back(std::sin(static_cast<float>(i * 8 + d)));

        playlist.tracks.push_back(std::move(track));
    }

    return playlist;
}

struct Profile
{
    std::string name;
    int level = 0;

    MIRO_REFLECT(name, level)
};
} // namespace

auto tBinaryRoundTrip = test("Format/binary document round trips bit-exact") = []
{
    const auto dir = TempDir {};
    const auto path = dir.file("data.ember");

    auto playlist = makePlaylist(20);

    {
        auto doc = Document<Playlist> {path, Format::Binary};
        check(doc.set(playlist));
    }

    // A fresh document (no warm cache) must reload the same bytes.
    auto reopened = Document<Playlist> {path, Format::Binary};
    const auto loaded = reopened.get();

    check(loaded.name == playlist.name);
    check(loaded.tracks.size() == playlist.tracks.size());

    for (size_t i = 0; i < loaded.tracks.size(); ++i)
    {
        check(loaded.tracks[i].title == playlist.tracks[i].title);
        check(loaded.tracks[i].duration == playlist.tracks[i].duration);
        check(loaded.tracks[i].embedding == playlist.tracks[i].embedding);
    }

    check(startsWithMagic(readRaw(path)));
};

auto tBinaryReadsJson = test("Format/binary document reads a legacy JSON file") = []
{
    const auto dir = TempDir {};
    const auto path = dir.file("data.ember");

    // A JSON-format document wrote this file in a previous life.
    {
        auto doc = Document<Profile> {path, Format::Json};
        check(doc.set({"Alice", 3}));
    }
    check(!startsWithMagic(readRaw(path)));

    // The binary document reads it by sniffing the content...
    auto doc = Document<Profile> {path, Format::Binary};
    check(doc.get().name == "Alice");
    check(doc.get().level == 3);

    // ...and its next write converts the file.
    check(doc.mutate([](Profile& p) { p.level = 4; }));
    check(startsWithMagic(readRaw(path)));
    check(doc.get().level == 4);
};

auto tJsonReadsBinary = test("Format/json document reads a binary file") = []
{
    const auto dir = TempDir {};
    const auto path = dir.file("data.json");

    {
        auto doc = Document<Profile> {path, Format::Binary};
        check(doc.set({"Bob", 7}));
    }
    check(startsWithMagic(readRaw(path)));

    auto doc = Document<Profile> {path, Format::Json};
    check(doc.get().name == "Bob");

    check(doc.mutate([](Profile& p) { p.level = 8; }));
    check(!startsWithMagic(readRaw(path)));
    check(doc.get().level == 8);
};

auto tDatabaseFormatSwitch =
    test("Format/database adopts stores across format switches") = []
{
    const auto dir = TempDir {};

    // Day 1: a JSON database.
    {
        auto db = Database {dir.root.string()};
        auto users = db.collection<Profile>("users");
        check(users.doc("alice").set({"Alice", 1}));
        check(users.doc("bob").set({"Bob", 5}));
    }
    check(fs::exists(dir.root / "users.json"));

    // Day 2: same directory opened as a binary database. The store file is
    // adopted under the new extension and stays fully readable.
    {
        auto db = Database {dir.root.string(), Format::Binary};
        auto users = db.collection<Profile>("users");

        check(users.size() == 2);
        check(users.doc("alice").get()->name == "Alice");
        check(!fs::exists(dir.root / "users.json"));
        check(fs::exists(dir.root / "users.ember"));

        // Content is still the old JSON until something writes...
        check(!startsWithMagic(readRaw(dir.file("users.ember"))));

        // ...and converts on the first write.
        check(users.doc("carol").set({"Carol", 9}));
        check(startsWithMagic(readRaw(dir.file("users.ember"))));
    }

    // Day 3: back to JSON — adopted back, binary content sniffed, converted
    // on write.
    {
        auto db = Database {dir.root.string()};
        auto users = db.collection<Profile>("users");

        check(users.size() == 3);
        check(users.doc("carol").get()->level == 9);
        check(fs::exists(dir.root / "users.json"));
        check(!fs::exists(dir.root / "users.ember"));

        check(users.doc("carol").remove());
        check(!startsWithMagic(readRaw(dir.file("users.json"))));
        check(users.size() == 2);
    }
};

auto tBinaryQueries = test("Format/queries work unchanged on a binary store") = []
{
    const auto dir = TempDir {};
    auto db = Database {dir.root.string(), Format::Binary};
    auto users = db.collection<Profile>("users");

    for (auto i = 0; i < 20; ++i)
        check(users.doc("user" + std::to_string(i)).set({"U", i}));

    const auto strong = users.where([](const Profile& p) { return p.level >= 15; })
                            .orderBy(&Profile::level)
                            .get();

    check(strong.size() == 5);
    check(strong.front().level == 15);
    check(strong.back().level == 19);
};

auto tInsertAppends =
    test("Journal/inserts append frames instead of rewriting the file") = []
{
    const auto dir = TempDir {};
    auto db = Database {dir.root.string(), Format::Binary};
    auto users = db.collection<Profile>("users");

    for (auto i = 0; i < 100; ++i)
        check(users.doc("user" + std::to_string(i)).set({"Name", i}));

    const auto sizeBefore = fs::file_size(dir.file("users.ember"));
    check(users.doc("newcomer").set({"New", 1}));
    const auto sizeAfter = fs::file_size(dir.file("users.ember"));

    // One more insert costs one small frame, not a rewrite of 101 entries.
    check(sizeAfter > sizeBefore);
    check(sizeAfter - sizeBefore < 100);

    // A fresh handle replays the journal to the same map.
    auto reopened =
        Database {dir.root.string(), Format::Binary}.collection<Profile>("users");
    check(reopened.size() == 101);
    check(reopened.doc("newcomer").get()->name == "New");
    check(reopened.doc("user42").get()->level == 42);
};

auto tRemoveAppends = test("Journal/removes append a tombstone") = []
{
    const auto dir = TempDir {};
    auto db = Database {dir.root.string(), Format::Binary};
    auto users = db.collection<Profile>("users");

    for (auto i = 0; i < 50; ++i)
        check(users.doc("user" + std::to_string(i)).set({"Name", i}));

    const auto sizeBefore = fs::file_size(dir.file("users.ember"));
    check(users.doc("user7").remove());
    const auto sizeAfter = fs::file_size(dir.file("users.ember"));

    check(sizeAfter > sizeBefore);
    check(sizeAfter - sizeBefore < 50);

    auto reopened =
        Database {dir.root.string(), Format::Binary}.collection<Profile>("users");
    check(reopened.size() == 49);
    check(!reopened.doc("user7").exists());

    // Removing an absent key is a no-op that touches nothing on disk.
    check(users.doc("ghost").remove());
    check(fs::file_size(dir.file("users.ember")) == sizeAfter);
};

auto tJournalCompaction =
    test("Journal/compaction keeps a churned store bounded") = []
{
    const auto dir = TempDir {};
    auto db = Database {dir.root.string(), Format::Binary};
    auto users = db.collection<Profile>("users");

    // Rewrite the same handful of keys thousands of times: without
    // compaction this journal would grow without bound.
    for (auto i = 0; i < 3000; ++i)
        check(users.doc("user" + std::to_string(i % 5)).set({"Churn", i}));

    // The tail may reach the compaction limit (max(base, 4096)) plus one
    // frame before it folds; leave generous headroom.
    check(fs::file_size(dir.file("users.ember")) < 16 * 1024);

    auto reopened =
        Database {dir.root.string(), Format::Binary}.collection<Profile>("users");
    check(reopened.size() == 5);
    check(reopened.doc("user4").get()->level == 2999);
};

auto tJournalTornTail = test("Journal/a torn append is dropped and healed") = []
{
    const auto dir = TempDir {};
    const auto path = dir.file("users.ember");

    {
        auto db = Database {dir.root.string(), Format::Binary};
        auto users = db.collection<Profile>("users");
        for (auto i = 0; i < 10; ++i)
            check(users.doc("user" + std::to_string(i)).set({"Name", i}));
    }

    // A crash mid-append: garbage lands after the last valid frame.
    {
        auto out = std::ofstream {path, std::ios::binary | std::ios::app};
        out << "\x01garbage-torn-frame";
    }

    // All complete frames survive the torn tail...
    auto db = Database {dir.root.string(), Format::Binary};
    auto users = db.collection<Profile>("users");
    check(users.size() == 10);
    check(users.doc("user9").get()->level == 9);

    // ...and the next write compacts, leaving a clean journal that still
    // holds everything (appending after the garbage would hide the new
    // frame behind it, so the store rewrites instead).
    check(users.doc("healed").set({"Healed", 1}));

    auto reopened =
        Database {dir.root.string(), Format::Binary}.collection<Profile>("users");
    check(reopened.size() == 11);
    check(reopened.doc("user9").get()->level == 9);
    check(reopened.doc("healed").get()->name == "Healed");
};

auto tInsertSpeed =
    test("Journal/appending inserts beat JSON rewrites (timings printed)") = []
{
    const auto dir = TempDir {};
    constexpr auto docCount = 300;

    const auto timedInserts = [&](auto&& collection)
    {
        const auto start = std::chrono::steady_clock::now();
        for (auto i = 0; i < docCount; ++i)
            check(collection.doc("user" + std::to_string(i)).set({"Name", i}));
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - start)
            .count();
    };

    auto jsonDb = Database {dir.root.string(), Durability::Atomic};
    auto binaryDb = Database {dir.root.string(), Format::Binary, Durability::Atomic};

    const auto jsonMs = timedInserts(jsonDb.collection<Profile>("json_users"));
    const auto binaryMs = timedInserts(binaryDb.collection<Profile>("binary_users"));

    std::cout << docCount << " single-doc inserts:\n"
              << "  json (rewrite per insert):   " << jsonMs << " ms\n"
              << "  binary (append per insert):  " << binaryMs << " ms\n";

    // Appending a frame does O(1) work where the JSON store rewrites the
    // whole growing file every insert.
    check(binaryMs < jsonMs);
};

auto tBinarySizeAndSpeed =
    test("Format/binary store is smaller than JSON (timings printed)") = []
{
    const auto dir = TempDir {};
    const auto playlist = makePlaylist(5000);

    const auto timedWrite = [&](auto& doc)
    {
        const auto start = std::chrono::steady_clock::now();
        check(doc.set(playlist));
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - start)
            .count();
    };

    const auto timedReload = [&](const std::string& path, Format format)
    {
        auto fresh = Document<Playlist> {path, format};
        const auto start = std::chrono::steady_clock::now();
        check(fresh.get().tracks.size() == playlist.tracks.size());
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - start)
            .count();
    };

    auto jsonDoc = Document<Playlist> {dir.file("data.json"), Format::Json};
    auto binaryDoc = Document<Playlist> {dir.file("data.ember"), Format::Binary};

    const auto jsonWriteMs = timedWrite(jsonDoc);
    const auto binaryWriteMs = timedWrite(binaryDoc);
    const auto jsonReadMs = timedReload(dir.file("data.json"), Format::Json);
    const auto binaryReadMs = timedReload(dir.file("data.ember"), Format::Binary);

    const auto jsonSize = fs::file_size(dir.file("data.json"));
    const auto binarySize = fs::file_size(dir.file("data.ember"));

    std::cout << "Store of 5000 tracks:\n"
              << "  json:   " << jsonSize / 1024 << " KB, write " << jsonWriteMs
              << " ms, load " << jsonReadMs << " ms\n"
              << "  binary: " << binarySize / 1024 << " KB, write " << binaryWriteMs
              << " ms, load " << binaryReadMs << " ms\n";

    check(binarySize < jsonSize);
};
