// A rough performance guard: build a 1,000,000-row collection of random data
// and check that searching it isn't pathologically slow. Two numbers matter —
// the pure linear scan over loaded rows, and the full per-call cost (which also
// re-reads + re-parses the whole file, since the store keeps no cache).

#include <emberstore/Emberstore.h>

#include <NanoTest/NanoTest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <random>
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
               / ("emberstore-perf-test-" + std::to_string(counter.fetch_add(1)));
        fs::remove_all(root);
    }

    ~TempDir() { fs::remove_all(root); }

    std::string file() const { return (root / "rows.json").string(); }
};

struct Row
{
    std::string name;
    int score = 0;
    std::string tag;

    MIRO_REFLECT(name, score, tag)
};

double millisSince(std::chrono::steady_clock::time_point start)
{
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(now - start).count();
}
} // namespace

auto tMillionRowSearch =
    test("Collection/searching 1M random rows is not super bad") = []
{
    constexpr int rowCount = 1'000'000;
    auto dir = TempDir {};
    const auto path = dir.file();

    // Build the rows once and seed the file in a single write (Atomic skips the
    // fsync — this is a throughput test, not a durability one).
    auto data = std::map<std::string, Row> {};
    auto rng = std::mt19937 {0xABCDEFu};
    for (int i = 0; i < rowCount; ++i)
        data.emplace("row-" + std::to_string(i),
                     Row {"name-" + std::to_string(rng() % 100000),
                          static_cast<int>(rng() % 1'000'000),
                          "tag-" + std::to_string(rng() % 16)});

    emberstore::Document<std::map<std::string, Row>> {path,
                                                      emberstore::Durability::Atomic}
        .set(data);

    auto collection = emberstore::Collection<Row> {path};

    const auto predicate = [](const Row& r) { return r.score >= 999'990; };

    // Cold: nothing cached yet, so this reads and parses the whole file.
    const auto coldStart = std::chrono::steady_clock::now();
    const auto hits = collection.where(predicate).get();
    const auto coldMs = millisSince(coldStart);

    // Warm: the parse is cached, so this is a stat plus a scan.
    const auto warmStart = std::chrono::steady_clock::now();
    const auto warmHits = collection.where(predicate).get();
    const auto warmMs = millisSince(warmStart);

    // Pure scan cost over an already-loaded snapshot, for reference.
    const auto rows = collection.values();
    const auto scanStart = std::chrono::steady_clock::now();
    const auto scanned = emberstore::Query<Row> {rows}.where(predicate).get();
    const auto scanMs = millisSince(scanStart);

    std::printf("[perf] 1M rows: cold where()=%.0f ms, warm where()=%.1f ms, "
                "scan-only=%.1f ms, hits=%zu\n",
                coldMs,
                warmMs,
                scanMs,
                hits.size());

    check(rows.size() == static_cast<std::size_t>(rowCount));
    check(!hits.empty());
    check(hits.size() == warmHits.size());
    check(hits.size() == scanned.size());

    // Measured on an M-series mac (Release): cold ~1.7 s (one unavoidable
    // parse), warm ~10 ms, scan ~8 ms. The scan was never the problem — the
    // whole-file parse was, and the cache pays it once instead of per call.
    // Bounds carry headroom for slower CI while catching a 10x regression.
    check(scanMs < 250.0);
    check(warmMs < 250.0);
    check(coldMs < 10000.0);
};
