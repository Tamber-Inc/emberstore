# 0001 Renamed fields are migrated in the struct's own reflect()

**Status:** Accepted, 2026-11-11. Landed: `legacy()` removed from Miro in jamierpond/Miro 5fb1626.

**Problem.** Renaming a field lost its data: an old file's `field2` was never read into the new `fieldTwo`.

**Options.**
1. A Miro hook, `ref["fieldTwo"].legacy("field2")(fieldTwo)`, one old name per field (asserted on a second call).
2. Nothing in Miro: the struct's `reflect()` reads the old key while loading, then the new one.

**Decision.** Option 2. Jamie: "reflect doesn't need this users can manually impl their legacy patterns ... miro doesn't need to add this for correctness."

**Evidence.** A scratch check (macprorack, Release, emberstore pinned to Miro 5fb1626) loaded an old file through a hand-written `reflect()` in JSON and binary: old file migrates, the new key wins when both exist, saving writes only the new key. A renamed list or map only migrates this way with 0004's fix.

```cpp
void reflect(Miro::Reflector& ref)
{
    if (ref.isLoading())
        ref["field2"](fieldTwo);

    ref["field1"](field1);
    ref["fieldTwo"](fieldTwo);
}
```
