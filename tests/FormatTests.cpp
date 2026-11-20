// Tests for Format::Binary — the packed Miro wire format as a storage
// encoding — and for content sniffing: reads decode whatever the file
// actually holds, regardless of the configured format. Includes a size/speed comparison against
// the JSON encoding, printed for inspection; only the invariants are asserted.

#include <emberstore/Emberstore.h>

#include <NanoTest/NanoTest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
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

// Either binary magic: "CLD1" (plain document) or "CLDJ" (journal).
bool startsWithMagic(const std::string& bytes)
{
    return bytes.size() >= 4
           && (bytes.compare(0, 4, "CLD1") == 0 || bytes.compare(0, 4, "CLDJ") == 0);
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

// Two handles on one journal in one process: B appends (and on POSIX keeps
// its append handle open), then A writes past the compaction limit, which
// renames over the file. On Windows any open append handle made that rename
// fail, so this is the scenario AppendFile::persistent exists for.
auto tCompactionWithTwoHandles =
    test("Journal/compaction succeeds while another handle has appended") = []
{
    const auto dir = TempDir {};
    auto a =
        Database {dir.root.string(), Format::Binary}.collection<Profile>("users");
    auto b =
        Database {dir.root.string(), Format::Binary}.collection<Profile>("users");

    check(a.doc("seed").set({"Seed", 0})); // first write: a fresh journal base
    check(b.doc("fromB").set({"B", 1})); // an append through B's handle

    const auto padding = std::string(200, 'x');
    auto compacted = false;
    for (auto i = 0; i < 100; ++i)
    {
        const auto before = fs::file_size(dir.file("users.ember"));
        check(a.doc("a" + std::to_string(i)).set({padding, i}));
        compacted = compacted || fs::file_size(dir.file("users.ember")) < before;
    }
    check(compacted);

    // B picks up A's compacted file and can keep appending to it.
    check(b.doc("fromB2").set({"B", 2}));
    auto reopened =
        Database {dir.root.string(), Format::Binary}.collection<Profile>("users");
    check(reopened.size() == 103);
    check(reopened.doc("fromB").get()->level == 1);
    check(reopened.doc("a99").get()->level == 99);
};

// Schema evolution through the store: a collection written as ProfileV1
// is opened as ProfileV2, which adds a field. Entries written by V1 stay in
// the journal base without it; V2 writes land beside them, and a later V2
// reader sees both.
namespace
{
struct ProfileV1
{
    std::uint32_t id = 0;
    std::string displayName;

    MIRO_REFLECT(id, displayName)
};

struct ProfileV2
{
    std::uint32_t id = 0;
    std::string displayName;
    int karma = 10;

    MIRO_REFLECT(id, displayName, karma)
};
} // namespace

auto tSchemaEvolution =
    test("Format/binary collection written as V1 migrates when read as V2") = []
{
    const auto dir = TempDir {};

    {
        auto v1 = Database {dir.root.string(), Format::Binary}.collection<ProfileV1>(
            "profiles");

        for (auto i = 0u; i < 20; ++i)
            check(v1.set("p" + std::to_string(i), {i, "user" + std::to_string(i)}));

        check(v1.remove("p3"));
    }

    {
        auto v2 = Database {dir.root.string(), Format::Binary}.collection<ProfileV2>(
            "profiles");

        check(v2.ids().size() == 19);
        const auto p7 = v2.get("p7");
        check(p7.has_value());
        check(p7 && p7->id == 7);
        check(p7 && p7->displayName == "user7");
        check(p7 && p7->karma == 10);

        auto bumped = *p7;
        bumped.karma = 99;
        check(v2.set("p7", bumped));
        check(v2.set("new", {100, "fresh", 5}));
    }

    auto v2 = Database {dir.root.string(), Format::Binary}.collection<ProfileV2>(
        "profiles");
    check(v2.ids().size() == 20);
    check(!v2.contains("p3"));

    const auto p0 = v2.get("p0");
    check(p0 && p0->displayName == "user0" && p0->karma == 10);

    const auto p7 = v2.get("p7");
    check(p7 && p7->displayName == "user7" && p7->karma == 99);

    const auto fresh = v2.get("new");
    check(fresh && fresh->id == 100 && fresh->displayName == "fresh"
          && fresh->karma == 5);

    for (auto i = 0u; i < 20; ++i)
    {
        if (i == 3)
            continue;

        const auto entry = v2.get("p" + std::to_string(i));
        check(entry && entry->id == i
              && entry->displayName == "user" + std::to_string(i));
    }
};

namespace
{
Calder::Detail::JournalInfo inspectJournalFile(const std::string& path)
{
    return Calder::Detail::inspectJournal(detail::asBytes(readRaw(path)));
}

std::size_t countJournalKeys(const std::string& path)
{
    return Calder::Detail::readJournalKeyTable(detail::asBytes(readRaw(path))).getKeyCount();
}
} // namespace

auto tJournalNewFieldName =
    test("Journal/a write with a new field name compacts once, then appends") = []
{
    const auto dir = TempDir {};
    const auto path = dir.file("profiles.ember");
    constexpr auto entryCount = 5U;
    constexpr auto profileV1KeyCount = std::size_t {2};
    constexpr auto profileV2KeyCount = std::size_t {3};

    {
        auto v1 = Collection<ProfileV1> {path, Format::Binary, Durability::Atomic};

        for (auto i = 0U; i < entryCount; ++i)
            check(v1.set("v1-" + std::to_string(i), {i, "Old"}));
    }

    check(countJournalKeys(path) == profileV1KeyCount);

    auto v2 = Collection<ProfileV2> {path, Format::Binary, Durability::Atomic};
    check(v2.set("v2-0", {0, "New", 0}));

    const auto compacted = inspectJournalFile(path);
    check(compacted.frameBytes == 0);
    check(countJournalKeys(path) == profileV2KeyCount);

    for (auto i = 1U; i < entryCount; ++i)
        check(v2.set("v2-" + std::to_string(i), {i, "New", static_cast<int>(i)}));

    const auto appended = inspectJournalFile(path);
    check(appended.baseSize == compacted.baseSize);
    check(appended.frameBytes > 0);

    auto reopened = Collection<ProfileV2> {path, Format::Binary, Durability::Atomic};
    check(reopened.size() == 2 * entryCount);
    check(reopened.get("v2-3")->karma == 3);
    check(reopened.get("v1-2")->displayName == "Old");
    check(reopened.get("v1-2")->karma == 10);
};

// The two ways of stamping a file must agree on its identity, and a
// same-size replace must change it even inside one mtime tick.
auto tStampIdentity = test("Journal/path and handle stamps agree on identity") = []
{
    const auto dir = TempDir {};
    const auto path = dir.file("stamped.bin");
    check(writeFileAtomically(path, "aaaa", Durability::Atomic));

    auto file = AppendFile {};
    check(file.open(path));
    check(file.append("bb", Durability::Atomic));
    const auto fromHandle = file.stamp();
    file.close();

    const auto fromPath = detail::stampFile(path);
    check(fromHandle.exists);
    check(fromHandle == fromPath);

    check(writeFileAtomically(path, "cccccc", Durability::Atomic));
    const auto replaced = detail::stampFile(path);
    check(replaced.size == fromPath.size);
    check(replaced.fileId != fromPath.fileId || replaced.device != fromPath.device);
    check(!(replaced == fromPath));
};
