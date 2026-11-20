> Warning! This is alpha software. This is subject to change and may break your
> code. Use at your own risk. Bugs likely.

# emberstore

Embedded document store for [Miro](https://github.com/eyalamirmusic/Miro) types — SQLite-shaped, Firestore-flavoured. Writes are atomic (temp + rename, optional fsync).

```cpp
auto db = emberstore::Database {configDir};
db.document<Settings>("settings").set(s);          // whole-value get/set
auto users = db.collection<Profile>("users");      // keyed docs
users.doc("alice").set({"Alice", 1});
users.where([](auto& p){ return p.level > 2; }).orderBy(&Profile::level).get();
```

## Storage format

Stores are JSON files by default. Pass `Format::Binary` to write
[Calder](https://github.com/jamierpond/Calder), a packed binary format, instead — several times smaller and faster (a 5000-track
store: 321 KB vs 1.2 MB, ~8x faster writes, ~4x faster loads), with floats
stored bit-exact rather than as decimal text.

```cpp
auto db = emberstore::Database {configDir, emberstore::Format::Binary};
```

Binary collections also write differently: `set` / `remove` on one document
appends a checksummed journal frame to the file instead of rewriting the whole
collection (~10x faster inserts at 300 docs, and the gap grows with size). The
store compacts automatically once the appended tail outgrows the base, so the
file stays within ~2x of its compacted size; a torn append (crash mid-write)
is detected on load and dropped, and the next write heals the file.

Format is fixed per database: a store opened under the other format is a different file (`.json` vs `.ember`).

**Limits:** message-thread only (asserts); whole-file reads + linear-scan queries, so it's for modest data, not a large indexed DB. (Binary-format collections append single-doc writes instead of rewriting the file — see below.) Writes take an advisory inter-process lock (`FileLock`), and `LiveDocument` can republish external changes via a native `FileWatcher` (macOS + Windows).

## Using it

```cmake
CPMAddPackage(NAME emberstore GITHUB_REPOSITORY tamber/emberstore GIT_TAG main)
target_link_libraries(your_app PRIVATE emberstore)
```

Depends on [eacp](https://github.com/eyalamirmusic/eacp) and [Miro](https://github.com/eyalamirmusic/Miro), both fetched via [CPM](https://github.com/cpm-cmake/CPM.cmake) if not already in the build.

```cpp
#include <emberstore/Emberstore.h>
```

To develop against a local Miro checkout instead of the GitHub fetch, use
CPM's source override: `just test-local ../Miro` (or pass
`-DCPM_Miro_SOURCE=/path/to/Miro` to cmake yourself).

## Demo: firestore (Apps/Firestore)

An unsecured HTTP front end over a binary Database — no auth, demo only — for
feeling the write throughput with curl. Built as `firestore` when the repo is
top-level:

```bash
build-local/Apps/Firestore/firestore          # port 8080, ./firestore-data
curl -X PUT localhost:8080/d/notes/a -d '{"title":"hi","rating":5}'
curl localhost:8080/d/notes
curl -X POST 'localhost:8080/bench?n=2000'    # JSON store vs binary store
```

Every PUT reports its server-side cost in `writeMicros`; `/bench` races n
inserts through a fresh JSON store and a fresh binary journal store and
prints ms, writes/s and bytes on disk for each.

## Demo: sqlitestore (Apps/Sqlitestore)

The same app on SQLite (amalgamation via CPM, WAL mode, docs(col, id, body)
with the same JSON text as the body) — same routes, default port 8081. Its
`/bench` is the emberstore vs SQLite head-to-head:

```
2000 single-document inserts, no fsync (sqlite synchronous=OFF):
  sqlite          25.9 ms    77105 writes/s   356 KB
  ember json    3248.3 ms      615 writes/s   356 KB
  ember binary    14.8 ms   135396 writes/s   242 KB

2000 point reads (typed — parsed to a Note):
  sqlite           5.8 ms   345448 reads/s
  ember json       1.9 ms  1047509 reads/s
  ember binary     1.9 ms  1028696 reads/s

500 single-document inserts, fsync every write (sqlite synchronous=FULL):
  sqlite          49.6 ms    10088 writes/s    96 KB
  ember json     281.8 ms     1774 writes/s    88 KB
  ember binary    16.8 ms    29827 writes/s    63 KB
```

`Durability` maps to `PRAGMA synchronous` (Atomic = OFF, durable = FULL), so
both sides skip or pay for fsync together. The write gap comes from the
journal's persistent append handle (one write syscall per frame — see
`AppendFile`; POSIX only, Windows reopens per append); reads answer from the stamped in-memory cache, one stat plus
a map find.

## Demo: wiresize (Apps/Wiresize)

A CLI that generates a few data shapes (flat settings, 64-float embeddings,
nested sections of notes, a `map<string, Profile>` collection) at scales up to
`--max` (default 10000) and compares compact JSON with emberstore binary in memory:
size, ratio, and ms per encode/decode. The `journal` rows are the binary
Collection's on-disk form (base) for 10000 entries against the JSON of the
same map, plus the size of one appended `set` frame. `--json` emits the rows
as a JSON array.

```
build-local/Apps/Wiresize/wiresize --max 10000
```

From an Apple Silicon Mac, Release:

```
shape                      n       json B     binary B  ratio   json enc  json dec   bin enc   bin dec frame B
settings                   1          110          113  0.97x      0.001     0.001     0.001     0.001
settings                 100        11195         3960  2.83x      0.104     0.100     0.034     0.037
settings               10000      1119680       388737  2.88x      9.964    11.353     3.213     3.679
embeddings                 1          665          341  1.95x      0.012     0.004     0.000     0.000
embeddings               100        67994        29834  2.28x      1.184     0.399     0.024     0.025
embeddings             10000      6807736      2998618  2.27x    131.916    43.388     2.402     2.582
nested                     1          199          228  0.87x      0.001     0.001     0.001     0.001
nested                   100        14329        12653  1.13x      0.093     0.080     0.032     0.036
nested                 10000      1471699      1298753  1.13x      9.356    11.335     3.289     3.664
collection                 1           36           47  0.77x      0.000     0.000     0.000     0.000
collection               100         3771         2441  1.54x      0.041     0.055     0.016     0.025
collection             10000       416781       281405  1.48x      5.371     8.584     1.828     3.256
settings journal       10000      1218570       467631  2.61x                                               57
embeddings journal     10000      6906626      3077512  2.24x                                              315
nested journal         10000     15025561     13275619  1.13x                                             1361
collection journal     10000       416781       281413  1.48x                                               39
```

(Rows for n = 10 and 1000 trimmed.) Numeric data is where binary wins most —
floats are 4 raw bytes instead of ~10 characters, and encoding skips float
formatting entirely (~50x faster). String-heavy nested documents barely
shrink. Single tiny values are slightly larger in binary: the header and
length prefixes cost more than the field names they save.
