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

**Limits:** message-thread only (asserts); whole-file read/write per call + linear-scan queries, so it's for modest data, not a large indexed DB. Writes take an advisory inter-process lock (`FileLock`), and `LiveDocument` can republish external changes via a native `FileWatcher` (macOS + Windows).

## Batching writes

Every write goes through one commit path: take the file's inter-process lock, re-read the file if the change depends on it, apply the change and write the file once. `Database` commits after every `set`, `remove` and `mutate`, like SQLite's autocommit. `TransactionalDatabase` hands out the same `Document` and `Collection` stores, but keeps their changes in memory until you call `commit()`, which replays them under the lock and writes each changed file once. A staged `mutate` keeps the function itself, so at commit it runs on whatever the file then holds, and other processes' changes to other keys survive. Reads through the stores see your staged changes; other processes see nothing until the commit, and anything still staged when the database is destroyed is thrown away.

```cpp
auto db = emberstore::TransactionalDatabase {configDir};
auto& users = db.collection<Profile>("users");  // owned by db: hold a reference
users.doc("alice").set({"Alice", 1});
users.doc("bob").remove();

if (userCancelled)
    db.discardChanges();                         // nothing is written
else
    db.commit();                                 // one locked write for users.json
```

To hand out ids across processes, use a plain `Database` on the same folder: its `mutate` takes the lock, re-reads, increments and writes at once, whatever is staged elsewhere.

## Using it

```cmake
CPMAddPackage(NAME emberstore GITHUB_REPOSITORY tamber/emberstore GIT_TAG main)
target_link_libraries(your_app PRIVATE emberstore)
```

Depends on [eacp](https://github.com/eyalamirmusic/eacp) and [Miro](https://github.com/eyalamirmusic/Miro), both fetched via [CPM](https://github.com/cpm-cmake/CPM.cmake) if not already in the build.

```cpp
#include <emberstore/Emberstore.h>
```
