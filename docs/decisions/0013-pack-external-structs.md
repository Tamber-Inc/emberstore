# 0013 Pack number-only structs reflected with MIRO_REFLECT_EXTERNAL

**Status:** Accepted, 2026-11-16. Jamie, 2026-11-16: "yes 0009, 0013 sounds smart. 0014 sounds like good exploration." Landed 2026-11-16: Miro `jp/packed-structs` 2996897, in draft PR [eyalamirmusic/Miro#32](https://github.com/eyalamirmusic/Miro/pull/32) with 0009; the same change on `jp/packed-structs-on-hooks` (8180bbc), which Calder (`188ea17`) and emberstore pin.

**Problem.** 0009 packs arrays of number-only structs that use `MIRO_REFLECT`, but eacp's `Vec3` is reflected with `MIRO_REFLECT_EXTERNAL`, which gives Miro no field layout, so `vector<Vec3>` isn't packed unless someone hand-writes `MIRO_PACKED(eacp::Maths::Vec3, x, "x", y, "y", z, "z")`.

**Evidence.** physicsgame2, 1k entities with baked rock geometry: 142 ms to load without the declaration, 25 ms with it (macprorack, Release). Script: `~/projects/physicsgame2/scripts/scene-load-benchmark.sh`.

**Idea.** Have `MIRO_REFLECT_EXTERNAL` emit the same compile-time field types, names and offsets as `MIRO_REFLECT`, so external number structs pack with no declaration and no call-site change.

**How it landed.** `MIRO_REFLECT_EXTERNAL` and `MIRO_REFLECT_EXTERNAL_MEMBERS` specialize a new `Miro::ExternalFields<Type>` with the same field descriptions `MIRO_REFLECT` emits; `PackedStruct.h` reads it beside `PackedFields` (`MIRO_PACKED`, which still wins if both exist). Its `describeFields()` is a template so `offsetof` on a non-standard-layout external type is never instantiated. Tests: Miro 873/873 (an external `Vec3` packs, `_MEMBERS` packs under its keys, an external struct with a string walks); Calder 110/110 (an external `Vec3` list writes the same bytes as a `MIRO_REFLECT` one).
