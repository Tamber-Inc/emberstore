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

## Using it

```cmake
CPMAddPackage(NAME emberstore GITHUB_REPOSITORY tamber/emberstore GIT_TAG main)
target_link_libraries(your_app PRIVATE emberstore)
```

Depends on [eacp](https://github.com/eyalamirmusic/eacp) and [Miro](https://github.com/eyalamirmusic/Miro), both fetched via [CPM](https://github.com/cpm-cmake/CPM.cmake) if not already in the build.

```cpp
#include <emberstore/Emberstore.h>
```
