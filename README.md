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

`Database` writes the file on every `set` and `remove`. `TransactionalDatabase` has the same `collection` and `document` calls, but keeps changes in memory until you call `commit()`, which writes each changed file once under its lock. Reads through it see your staged changes; other processes see nothing until the commit. Changes to different keys from other processes survive your commit, and for the same key the last commit wins. Anything still staged when the database is destroyed is thrown away.

```cpp
auto db = emberstore::TransactionalDatabase {configDir};
auto users = db.collection<Profile>("users");
users.doc("alice").set({"Alice", 1});
users.doc("bob").remove();

if (userCancelled)
    db.discardChanges();               // nothing is written
else
    db.commit();                       // one locked write for users.json

// Writes at once, even with other changes staged: the way to hand out ids.
db.document<IdCounter>("ids").mutateImmediately([&](IdCounter& c) { id = c.next++; });
```

## Using it

```cmake
CPMAddPackage(NAME emberstore GITHUB_REPOSITORY tamber/emberstore GIT_TAG main)
target_link_libraries(your_app PRIVATE emberstore)
```

Depends on [eacp](https://github.com/eyalamirmusic/eacp) and [Miro](https://github.com/eyalamirmusic/Miro), both fetched via [CPM](https://github.com/cpm-cmake/CPM.cmake) if not already in the build.

```cpp
#include <emberstore/Emberstore.h>
```
