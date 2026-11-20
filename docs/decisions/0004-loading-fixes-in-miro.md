# 0004 Missing lists/maps keep their defaults; numbers that don't fit are refused

**Status:** Accepted, 2026-11-15. Jamie: "Yes please make separate prs for both fixes please". PRs: eyalamirmusic/Miro#27 (missing lists/maps keep their defaults) and #28 (numbers that don't fit are refused), both drafts off develop. emberstore pins `jp/reflector-hooks` (e102944) until they land.

**Problem.** Loading a file written by an older struct damaged data in every format, JSON included:
- A list or map field added with a non-empty default was emptied when the old file didn't have the key.
- A number that no longer fit its field (int64 into int32, 1e300 into int) was cast to garbage.

**Why it must be in Miro.** Miro's vector code resizes to `arraySize()`, which is 0 for a missing key, so no reflector can stop the clear. And for `int16_t`, `uint32_t` and other non-`int` fields, Miro widens to int64 before the reflector sees the value, so only Miro knows the real field type to range-check against.

**Evidence.** #27: 2 of its 3 tests fail without the fix (`Missing containers: fields added in V2 keep their defaults`, `...nested, vector and map elements default added fields`). #28: 7 of its 14 tests fail without the fix (e.g. `Number range: int64 -> int32 out of range keeps prior`). emberstore's binary evolution tests depend on both.
