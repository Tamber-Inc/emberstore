// wiresize — JSON vs Miro binary, in memory, no disk.
//
//   wiresize [--max n] [--json]
//
// Generates a few data shapes at scales 1, 10, 100, ... up to --max (default
// 10000) and prints, per shape and scale, the encoded size in each format and
// the time to encode and decode it. Then one row per shape for the journal
// form a binary Collection writes: the compacted base vs the JSON of the same
// map, and the size of one appended entry frame.
//
// JSON is compact (indent 0) so the comparison is about the encoding; the
// stores' .json files are pretty-printed and larger still.

#include <emberstore/Emberstore.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace
{
// --- Shapes ---------------------------------------------------------------

// Flat: strings, ints, bools, one double.
struct Settings
{
    std::string theme;
    std::string language;
    int volume = 0;
    int sampleRate = 0;
    bool darkMode = false;
    bool autoSave = false;
    double zoom = 1.0;

    MIRO_REFLECT(theme, language, volume, sampleRate, darkMode, autoSave, zoom)
};

// Numeric-heavy: a 64-float embedding and a couple of doubles per element.
struct Track
{
    std::string title;
    double duration = 0.0;
    double loudness = 0.0;
    std::vector<float> embedding;

    MIRO_REFLECT(title, duration, loudness, embedding)
};

// Nested: sections of notes, each note carrying a vector of tags.
struct Note
{
    std::string title;
    std::string body;
    double rating = 0.0;
    std::vector<std::string> tags;

    MIRO_REFLECT(title, body, rating, tags)
};

struct Section
{
    std::string heading;
    std::vector<Note> notes;

    MIRO_REFLECT(heading, notes)
};

struct Notebook
{
    std::string name;
    std::vector<Section> sections;

    MIRO_REFLECT(name, sections)
};

// Collection: the map<string, Profile> the tests store.
struct Profile
{
    std::string name;
    int level = 0;

    MIRO_REFLECT(name, level)
};

Settings makeSettings(int i)
{
    return {i % 2 ? "dark" : "light",
            i % 3 ? "en-GB" : "de-DE",
            i % 101,
            i % 2 ? 48000 : 44100,
            i % 2 == 0,
            i % 5 != 0,
            1.0 + (i % 7) * 0.125};
}

Track makeTrack(int i)
{
    auto track = Track {};
    track.title = "Track " + std::to_string(i);
    track.duration = 180.0 + i * 0.377;
    track.loudness = -14.0 + std::sin(i * 0.1);
    for (auto d = 0; d < 64; ++d)
        track.embedding.push_back(std::sin(static_cast<float>(i * 64 + d)));
    return track;
}

Note makeNote(int i)
{
    return {"Note " + std::to_string(i),
            "Body text for note " + std::to_string(i)
                + ", long enough to look like something a person typed.",
            (i % 50) / 10.0,
            {"tag" + std::to_string(i % 7), "inbox"}};
}

Profile makeProfile(int i) { return {"User " + std::to_string(i), i % 100}; }

// n notes, ten to a section.
Notebook makeNotebook(int n)
{
    auto book = Notebook {"bench", {}};
    for (auto i = 0; i < n; ++i)
    {
        if (i % 10 == 0)
            book.sections.push_back({"Section " + std::to_string(i / 10), {}});
        book.sections.back().notes.push_back(makeNote(i));
    }
    return book;
}

Section makeSection(int i)
{
    auto section = Section {"Section " + std::to_string(i), {}};
    for (auto j = 0; j < 10; ++j)
        section.notes.push_back(makeNote(i * 10 + j));
    return section;
}

// --- Measuring ------------------------------------------------------------

using Clock = std::chrono::steady_clock;

// Average ms per call, repeating small cases so they register on the clock.
template <typename Fn>
double timeMs(int reps, Fn&& fn)
{
    const auto start = Clock::now();
    for (auto i = 0; i < reps; ++i)
        fn();
    const auto total =
        std::chrono::duration<double, std::milli> {Clock::now() - start}.count();
    return total / reps;
}

struct Row
{
    std::string shape;
    std::string n;
    std::size_t jsonBytes = 0;
    std::size_t binaryBytes = 0;
    double jsonEncodeMs = 0, jsonDecodeMs = 0, binEncodeMs = 0, binDecodeMs = 0;
    std::size_t frameBytes = 0; // journal rows only
};

std::size_t sink = 0; // keeps decoded values from being optimized away

template <typename T>
Row measure(const std::string& shape, int n, const T& value)
{
    const auto reps = std::max(1, 20000 / std::max(1, n));
    auto row = Row {shape, std::to_string(n)};

    const auto json = Miro::toJSONString(value);
    const auto binary = Calder::toBinary(value);
    row.jsonBytes = json.size();
    row.binaryBytes = binary.size();

    row.jsonEncodeMs =
        timeMs(reps, [&] { sink += Miro::toJSONString(value).size(); });
    row.jsonDecodeMs = timeMs(reps,
                              [&]
                              {
                                  const auto back =
                                      Miro::createFromJSONString<T>(json);
                                  sink += sizeof(back);
                              });
    row.binEncodeMs =
        timeMs(reps, [&] { sink += Calder::toBinary(value).size(); });
    row.binDecodeMs = timeMs(reps,
                             [&]
                             {
                                 const auto back =
                                     Calder::createFromBinary<T>(binary);
                                 sink += sizeof(back);
                             });
    return row;
}

// The binary Collection's on-disk form for n entries vs the JSON of the same
// map, plus what one more set() appends.
template <typename T, typename Make>
Row measureJournal(const std::string& shape, int n, Make&& make)
{
    auto map = std::map<std::string, T> {};
    for (auto i = 0; i < n; ++i)
        map["id-" + std::to_string(i)] = make(i);

    auto row = Row {shape + " journal", std::to_string(n)};
    row.jsonBytes = Miro::toJSONString(map).size();
    const auto journal = Calder::Detail::makeJournal(Calder::toBinary(map));
    row.binaryBytes = journal.size();
    auto keyTable = Calder::Detail::readJournalKeyTable(journal);

    auto frame = Calder::Detail::makeJournalSetFrame(
        "id-" + std::to_string(n), make(n), keyTable);

    if (frame.hasFrame())
        row.frameBytes = frame.frame.size();
    return row;
}

void printTable(const std::vector<Row>& rows)
{
    std::printf("%-20s %7s %12s %12s %6s  %9s %9s %9s %9s %7s\n",
                "shape",
                "n",
                "json B",
                "binary B",
                "ratio",
                "json enc",
                "json dec",
                "bin enc",
                "bin dec",
                "frame B");

    for (const auto& r: rows)
    {
        const auto ratio = static_cast<double>(r.jsonBytes) / r.binaryBytes;
        const auto journal = r.frameBytes > 0;
        std::printf("%-20s %7s %12zu %12zu %5.2fx",
                    r.shape.c_str(),
                    r.n.c_str(),
                    r.jsonBytes,
                    r.binaryBytes,
                    ratio);
        if (journal)
            std::printf("  %9s %9s %9s %9s %7zu\n", "", "", "", "", r.frameBytes);
        else
            std::printf("  %9.3f %9.3f %9.3f %9.3f %7s\n",
                        r.jsonEncodeMs,
                        r.jsonDecodeMs,
                        r.binEncodeMs,
                        r.binDecodeMs,
                        "");
    }
    std::printf("\ntimes are ms per encode/decode, in memory; json is compact\n");
}

void printJson(const std::vector<Row>& rows)
{
    std::printf("[\n");
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        const auto& r = rows[i];
        std::printf("  {\"shape\": \"%s\", \"n\": %s, \"jsonBytes\": %zu, "
                    "\"binaryBytes\": %zu",
                    r.shape.c_str(),
                    r.n.c_str(),
                    r.jsonBytes,
                    r.binaryBytes);
        if (r.frameBytes > 0)
            std::printf(", \"frameBytes\": %zu}", r.frameBytes);
        else
            std::printf(", \"jsonEncodeMs\": %.4f, \"jsonDecodeMs\": %.4f, "
                        "\"binaryEncodeMs\": %.4f, \"binaryDecodeMs\": %.4f}",
                        r.jsonEncodeMs,
                        r.jsonDecodeMs,
                        r.binEncodeMs,
                        r.binDecodeMs);
        std::printf("%s\n", i + 1 < rows.size() ? "," : "");
    }
    std::printf("]\n");
}

template <typename Make>
auto vectorOf(int n, Make&& make)
{
    auto out = std::vector<decltype(make(0))> {};
    for (auto i = 0; i < n; ++i)
        out.push_back(make(i));
    return out;
}
} // namespace

int main(int argc, char** argv)
{
    auto maxN = 10000;
    auto asJson = false;

    for (auto i = 1; i < argc; ++i)
    {
        const auto arg = std::string {argv[i]};
        if (arg == "--json")
            asJson = true;
        else if (arg == "--max" && i + 1 < argc)
            maxN = std::max(1, std::atoi(argv[++i]));
        else
        {
            std::fprintf(stderr, "usage: wiresize [--max n] [--json]\n");
            return 2;
        }
    }

    auto scales = std::vector<int> {};
    for (auto n = 1; n <= maxN; n *= 10)
        scales.push_back(n);

    auto rows = std::vector<Row> {};
    for (const auto n: scales)
        rows.push_back(measure("settings", n, vectorOf(n, makeSettings)));
    for (const auto n: scales)
        rows.push_back(measure("embeddings", n, vectorOf(n, makeTrack)));
    for (const auto n: scales)
        rows.push_back(measure("nested", n, makeNotebook(n)));
    for (const auto n: scales)
    {
        auto map = std::map<std::string, Profile> {};
        for (auto i = 0; i < n; ++i)
            map["id-" + std::to_string(i)] = makeProfile(i);
        rows.push_back(measure("collection", n, map));
    }

    const auto journalN = scales.back();
    rows.push_back(measureJournal<Settings>("settings", journalN, makeSettings));
    rows.push_back(measureJournal<Track>("embeddings", journalN, makeTrack));
    rows.push_back(measureJournal<Section>("nested", journalN, makeSection));
    rows.push_back(measureJournal<Profile>("collection", journalN, makeProfile));

    if (asJson)
        printJson(rows);
    else
        printTable(rows);

    return sink == 0 ? 1 : 0;
}
