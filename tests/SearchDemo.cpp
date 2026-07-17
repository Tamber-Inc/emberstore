// A runnable demo of the collection's search surface: populate a collection,
// then run Firestore-shaped where / orderBy / limit queries and print results.

#include <emberstore/Emberstore.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace emberstore;

namespace
{
struct Sample
{
    std::string name;
    int bpm = 0;
    std::string key;
    std::string type;

    MIRO_REFLECT(name, bpm, key, type)
};

void printHeader(const char* title)
{
    std::printf("\n\033[1m%s\033[0m\n", title);
    std::printf("  %-16s %5s  %-4s %-6s\n", "name", "bpm", "key", "type");
    std::printf("  ------------------------------------------\n");
}

void printRows(const std::vector<Sample>& rows)
{
    for (const auto& s: rows)
        std::printf("  %-16s %5d  %-4s %-6s\n",
                    s.name.c_str(),
                    s.bpm,
                    s.key.c_str(),
                    s.type.c_str());
    if (rows.empty())
        std::printf("  (none)\n");
}
} // namespace

int main()
{
    const auto root = std::filesystem::temp_directory_path() / "emberstore-search-demo";
    std::filesystem::remove_all(root);

    auto db = emberstore::Database {root.string()};
    auto samples = db.collection<Sample>("samples");

    samples.doc("s1").set({"Deep Kick", 124, "Am", "kick"});
    samples.doc("s2").set({"Punch Kick", 128, "C", "kick"});
    samples.doc("s3").set({"Sub Boom", 118, "Am", "kick"});
    samples.doc("s4").set({"Tight Snare", 140, "G", "snare"});
    samples.doc("s5").set({"Rim Shot", 132, "Am", "snare"});
    samples.doc("s6").set({"Closed Hat", 126, "C", "hat"});
    samples.doc("s7").set({"Open Hat", 122, "Am", "hat"});
    samples.doc("s8").set({"Clap One", 130, "F", "clap"});

    std::printf(
        "Stored %zu documents at %s\n", samples.size(), db.directory().c_str());

    printHeader("where: kicks under 126 bpm");
    printRows(
        samples
            .where([](const Sample& s) { return s.type == "kick" && s.bpm < 126; })
            .get());

    printHeader("where + orderBy: key = Am, slowest first");
    printRows(samples.where([](const Sample& s) { return s.key == "Am"; })
                  .orderBy([](const Sample& s) { return s.bpm; })
                  .get());

    printHeader("orderBy desc + limit: 3 fastest samples");
    printRows(samples.query()
                  .orderBy([](const Sample& s) { return s.bpm; }, false)
                  .limit(3)
                  .get());

    if (const auto slowestHat =
            samples.where([](const Sample& s) { return s.type == "hat"; })
                .orderBy([](const Sample& s) { return s.bpm; })
                .first())
        std::printf("\nfirst(): slowest hat is %s at %d bpm\n",
                    slowestHat->name.c_str(),
                    slowestHat->bpm);

    const auto snareCount =
        samples.where([](const Sample& s) { return s.type == "snare"; }).count();
    std::printf("count(): %zu snares\n\n", snareCount);

    std::filesystem::remove_all(root);
    return 0;
}
