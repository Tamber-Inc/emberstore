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

Stores are JSON files by default. Pass `Format::Binary` to write Miro's packed
binary wire format instead — several times smaller and faster (a 5000-track
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

Switching format on an existing database just works: the old store file is
adopted under the new extension (`.json` ↔ `.ember`), reads sniff the actual
file content regardless of the configured format, and each store converts the
first time it writes. Flip back any time you want greppable files.

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
`AppendFile`); reads answer from the stamped in-memory cache, one stat plus
a map find.
