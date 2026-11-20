# 0016 Calder's public API: fun to use, hard to misuse

**Status:** Accepted, 2026-11-16; landed in Calder `98bbd57`. Jamie: "ok sounds good for now", after asking that "Calder.h should display the very essential api. not noise up understanding with helpers." Calder.h holds only the user-facing declarations, one line each; implementation lives in `Calder/Detail/`, included at the bottom; the README opens with the same block. `File::open` on a journal refuses for now; the magic and extension change in 0017. `LoadReport` is renamed `LoadResult` (Jamie: "load result").

**Problem.** Two independent reviews of Calder at 6c9db53 (`~/tmp/calder-api-review/`, `~/tmp/calder-idiot/`) found:
- A failed load looks like a successful one. Garbage, an empty buffer, a cut-short file and a different struct all load as a default value with a clean report; `createFromBinary` drops even that. In a 200,000-file corruption fuzz, 192,906 loads returned wrong data with a clean report.
- `Document` and `View` dangle in natural code: 5 of 6 lifetime probes crashed under AddressSanitizer (`auto v = Calder::Document {bytes}.root()["name"];` compiles and reads freed memory).
- Using journal A's key table on journal B swaps field values silently; appending after a torn write loses every later frame.
- A crafted 150 KB file (10,000 nested levels) crashes a Release build; a 1 MB file can ask for 4 GB.
- `View::asInt()` is undefined behaviour on a large double.
- No file functions: every caller hand-writes reading, writing and a `reinterpret_cast` to bytes.
- Decoding one slice (a `View`) into a struct needs four internal symbols.
- `Calder.h` exposes the wire format (tags, varints, `ByteWriter`, `InternedKeyTable`) and re-exports Miro types into `Calder::`.

**Proposal.** Three types users touch (`File`, `View`, `LoadReport`) plus a journal writer; everything else moves to `Calder/Format.h` for tools and other decoders. All safety comes from types and the failure path, not checks in hot loops.

```cpp
namespace Calder
{
// Any contiguous bytes: std::string, std::vector<std::uint8_t>, std::span<const std::byte>, ...
template <typename R>
concept ByteRange = /* contiguous range of char, unsigned char, std::uint8_t or std::byte */;

enum class LoadStatus { Loaded, FileMissing, NotCalderData, Damaged, OutOfMemory };

struct LoadReport
{
    bool hasLoaded() const;    // false: nothing was loaded, the value is untouched
    bool hasProblems() const;  // not loaded, or fields were skipped, refused or missing
    LoadStatus status = LoadStatus::Loaded;
    std::vector<FieldProblem> fieldProblems;   // wrong type, number didn't fit; first 64 kept
    std::size_t fieldProblemCount = 0;
    std::size_t missingFieldCount = 0;
};

// Whole values
template <typename T> std::vector<std::uint8_t> toBinary(const T& value);
template <typename T> [[nodiscard]] LoadReport fromBinary(T& value, const ByteRange auto& bytes);
template <typename T> [[nodiscard]] std::optional<T> createFromBinary(const ByteRange auto& bytes);

// Files (save writes a temporary file, then renames it over the old one)
template <typename T> [[nodiscard]] bool saveToFile(const T& value, const std::filesystem::path& file);
template <typename T> [[nodiscard]] LoadReport loadFromFile(T& value, const std::filesystem::path& file);
template <typename T> [[nodiscard]] std::optional<T> createFromFile(const std::filesystem::path& file);

// Reading in place: a File owns its bytes; its state lives on the heap, so moving a File keeps its Views valid
class View
{
public:
    bool isValid() const;
    bool isNull() const; bool isBool() const; bool isNumber() const;
    bool isString() const; bool isObject() const; bool isArray() const;
    View operator[](std::string_view key) const;
    View operator[](std::size_t index) const;
    std::size_t getSize() const;
    std::vector<std::string_view> getKeys() const;
    bool asBool(bool fallback = false) const;
    std::int64_t asInteger(std::int64_t fallback = 0) const;   // fallback if it doesn't fit
    double asNumber(double fallback = 0.0) const;
    std::string_view asString(std::string_view fallback = {}) const;
};

class File
{
public:
    static File open(const std::filesystem::path& file);     // maps the file; nothing decoded
    static File fromBytes(std::vector<std::uint8_t> bytes);
    File(const File&) = delete;                               // movable, not copyable
    LoadStatus getStatus() const;
    View getRoot() const&;
    View getRoot() const&& = delete;                         // no Views into a temporary File
};

template <typename T> [[nodiscard]] LoadReport fromBinary(T& value, const View& view);
template <typename T> [[nodiscard]] std::optional<T> createFromBinary(const View& view);  // nullopt if missing

// Journals: the key table lives inside the writer, so it can't be mixed up
class JournalWriter
{
public:
    static JournalWriter open(const std::filesystem::path& journal);   // trims a torn tail first
    template <typename T> [[nodiscard]] bool appendSet(std::string_view key, const T& value);
    [[nodiscard]] bool appendRemove(std::string_view key);
    template <typename Map> [[nodiscard]] bool rewrite(const Map& entries);   // compaction
};
}
```

```cpp
// What users write
Calder::saveToFile(save, "save.calder");

if (auto save = Calder::createFromFile<SaveGame>("save.calder"))
    startGame(*save);

auto world = Calder::File::open("world.calder");
if (auto chunk = Calder::createFromBinary<WorldChunk>(world.getRoot()["12_7"]))
    streamIn(*chunk);
```

**Also, on the failure path only (no hot-loop cost):** a nesting-depth limit in the reader (reported as `Damaged`); the mismatch list capped; numbers that don't fit are reported, not silently refused; Miro's `enumFromNumber` range-checked; `asInteger` uses `convertNumber`; journals replay into maps with any comparator.

**Not decided here.**
- `File::open` on a journal: refuse (`NotCalderData`) for now, or merge newest frames over the base. Journal contents load through `loadFromFile` into a map.
- Fast lookup of one key in a big map: 0014 measured a sorted key index (`IndexedMap`) at 0.06 ms per cell at 1M entities; it fits this API unchanged.
- The file magic (`MIB1`/`MIBJ`) and extension: rename to Calder's (`CLD1`, `.calder`) now, while nothing has shipped?
- Rejected from the reviews: letting journal writes carry new field names (that's what 0006's revision removed, so a corrupt write loses only itself). The `JournalWriter` hides the bookkeeping instead.

**Landed, 2026-11-16,** in jamierpond/Calder `98bbd57` (CI green on Linux clang, gcc and AddressSanitizer + UndefinedBehaviorSanitizer, macOS and Windows MSVC). emberstore and physicsgame2 pin it, on jamierpond/Miro `jp/calder-emberstore` `65d6c7b`. What each problem above became:
- Failed loads: every load returns `LoadResult` (renamed from `LoadReport`, see 0017) or `std::optional`. Statuses: Loaded, FileMissing, ValueMissing (a View with nothing behind it), NotCalderData, IsJournal (`File::open` refuses journals for now), Damaged, OutOfMemory. Missing fields, wrong types, refused numbers and lost journal frames are counted. Container sizes past the end, a root that does not end at the key table and unknown key ids are Damaged. The 200,000-file corruption fuzz was not rerun; a 2,000-file version runs in the tests under the sanitizers.
- Dangling Views: `File` owns its bytes on the heap; `getRoot()` on a temporary does not compile; move-assignment is deleted (it would free bytes Views read); clang warns on returning a View into a local File (`[[gsl::Pointer]]` plus `[[clang::lifetimebound]]`).
- Journals: `JournalWriter` keeps the key table, cuts a torn tail on open, adds new field names to the base's key table (so existing frames stay valid) instead of a full rewrite, and cuts a failed append back off. emberstore's store keeps its own journal code on `Calder::Detail` (it needs its lock, durability and compaction policy).
- Crafted files: nesting past 256 is Damaged; packed layouts and packed integer lists are bounded by their own payload; wide packed layouts load in linear time.
- `asInteger` never casts out of range. Files: `saveToFile` writes, fsyncs and renames, keeping the old file's permissions. Any contiguous bytes load without casts.
- Not done, need Miro: an enum loaded from an out-of-range number (`enumFromNumber`); a lone field narrower than `int` or `double` is refused inside Miro without a report; a 1-byte-per-element array loaded into a vector of big structs still allocates count × size (Calder can't see the element size).

