# 0002 Miro hands a whole array of fixed-width numbers to the reflector

**Status:** Accepted, 2026-11-14. PR: eyalamirmusic/Miro#26 (draft, head 1235b1a, CI 8/8 green), waiting for Eyal.

**Problem.** Miro gave a reflector a `std::vector<float>` one element at a time, through a temporary, so emberstore couldn't copy it as one block. The reflector can't recover the element type itself: only Miro's templated container code knows `T`.

**Options.**
1. Keep emberstore element-by-element and make each element cheaper (no Miro change).
2. `visitPacked(PackedArrayRef)` with typed spans for int32/int64/float/double.
3. The same for every fixed-width number type: int8 through uint64, float, double.
4. Raw bytes for any trivially copyable type (see 0003).

**Decision.** Option 3. Jamie: "yeah for all number types this would be great for now please, yes." The packable types are derived from the variant in one place: `concept PackableElement = std::constructible_from<PackedArrayRef::Variant, std::span<T>>` (Jamie: "put information in one place not two").

**Evidence.** emberstore, 1M elements, macprorack, Release (scratch benchmark, not in a repo):

| | packed | element by element |
|---|---|---|
| `vector<float>` save / load | 4.4 / 1.9 ms | 58.5 / 51.8 ms |
| `vector<double>` save / load | 3.0 / 5.1 ms | 46.5 / 51.3 ms |
| `vector<int32_t>` save / load | 4.5 / 5.7 ms | 40.7 / 53.3 ms |

**Not covered.** `bool`, `char`, `long double`, and `long long`/`long` where they aren't the `std::int64_t` type still go element by element.

**emberstore side.** Uncommitted in `~/projects/emberstore` (branch jp/binary): new fixed-width tags for 8- and 16-bit arrays (`PackedInt8`, `PackedUInt8`, `PackedInt16`, `PackedUInt16`), uint32/uint64 through the int64 tag. Pinned to jamierpond/Miro e102944 for now.
