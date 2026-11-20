# 0009 Pack arrays of simple reflected structs

**Status:** Accepted, 2026-11-16. Jamie, 2026-11-16: "yes 0009, 0013 sounds smart. 0014 sounds like good exploration." Supersedes 0003. Landed 2026-11-16: Calder `7e91943` (encoder, decoder, tests, SPEC.md §3.9 and example 11.7, Python decoder), Miro draft PR [eyalamirmusic/Miro#32](https://github.com/eyalamirmusic/Miro/pull/32) (`jp/packed-structs`, stacked on #26). Calder and emberstore now pin `jamierpond/Miro@8180bbc` (0013 on top of `46a67e2`) (`jp/packed-structs-on-hooks`: the same change on the reflector-hooks branch).

**Problem.** Game state is full of small structs of numbers (`Vec3`, `Quaternion`, `Color`, `Keyframe`). Each one is walked and stored field by field: a `Vec3` takes 23 bytes for 12 bytes of data, and per-field reflection dominates the 1 GB decode profile.

**Design.**
- A struct is packable when every field is a fixed-width number or a packable struct. This is decided at compile time: `MIRO_REFLECT(x, y, z)` also emits the field types (`decltype`), names and offsets (`offsetof`, instantiated once the class is complete). A hand-written `reflect()` opts in with `MIRO_PACKED(Type, (wireName, member), ...)`, checked the same way.
- Miro offers an array of packable structs to the reflector as one unit (base pointer, count, stride, field layout); the reflector may decline, as with `visitPacked`. JSON and XML decline.
- On the wire: the layout (names and number types) once per array, then the field values in reflect order, little-endian, no padding. Readers in any language see an array of objects (0008 holds).
- On load: one `memcpy` when the stored layout matches and the struct has no padding; otherwise copy field by field by offset, matching by name.
- Lone structs (not in an array) stay ordinary objects for now.

**Result.** macprorack, single-threaded, Release, load under 2 with other builds paused, medians of 3 runs over 2 alternating rounds (10 GB: one round, 1 GB chunks). (a) baseline, (b) 0007, (c) 0007 + packing. (b) and (c) encode the same world.

| size | build | on disk | serialize | deserialize |
|---|---|---|---|---|
| 1 GB | a | 1013 MB | 9.88 s | 9.29 s |
| | b | 1013 MB | 6.59 s | 7.64 s |
| | c | 931 MB | 4.93 s | 5.79 s |
| 2 GB | a | 2035 MB | 18.9 s | 17.5 s |
| | b | 2035 MB | 13.2 s | 14.5 s |
| | c | 1871 MB | 9.87 s | 11.8 s |
| 10 GB chunked | a | 10202 MB | 94.8 s | 87.3 s |
| | b | 10202 MB | 67.5 s | 73.9 s |
| | c | 9383 MB | 49.4 s | 57.9 s |

Packing makes this world about 25% faster to save, 18–24% faster to load and 8% smaller, on top of 0007. Every decoded world equals the original. Only `vector<Keyframe>` and `vector<Submesh>` pack here; the lone `Vec3`/`Quaternion` inside every `Transform` stay ordinary objects, which is the next win. What's left in the decode is the field-by-field walk of ordinary objects (`atKey`, `skipValue`, `spawnChild`, `readNextEntry`) and per-object type names (0011 fixes those).

**How it landed (experiment, not yet in this tree).** Miro: `PackedStruct.h`, `MIRO_REFLECT` detects number-only structs at compile time, `MIRO_PACKED(Type, member, "key", ...)` for hand-written reflect(), hook `visitPackedStructs(PackedStructArrayRef)`; branch `jp/packed-structs` (48781d3, +380/−14 in Lib), not pushed. emberstore: tag 18 PackedStructArray (field layout once as key-id paths and number types, then little-endian records); one `memcpy` when the layout matches, otherwise field by field by name. Spec §3.9 and the Python decoder updated. Tests: Miro 851/851, emberstore 171/172 (one flaky file-watch test passed 3/3 reruns), Python 47/47. Copy: `~/projects/emberstore-packed-structs`.

**How it landed in Calder.** The experiment's changes were taken as a diff against the 0007 copy it was built on (`~/projects/emberstore-reflector-speed`) and applied to Calder with no changes, except one forced by 0006: `Document::hasKeyId` checks the base document's key names when reading a journal frame. One test was added: a journal set frame writes and replays packed records. Calder 109/109 tests, Python decoder 52/52, emberstore 68/68 (the 1M-row timing test failed once under load and passed alone).
