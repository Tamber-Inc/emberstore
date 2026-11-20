# 0005 Store field names as 32/64-bit hashes

**Status:** Rejected, 2026-11-15. Jamie: "0005: reject hashing".

**Problem.** Long field names cost bytes: every journal write repeats its struct's names.

**What was tried.** FNV-1a hashes instead of names: no key table, each field writes its 4- or 8-byte hash. A collision inside one document is refused.

**Evidence.** macprorack, Release, ext4, medians of 3. Script: `~/projects/emberstore-key-hashing/Apps/Sqlitestore/bench-key-hashing.sh` (copy, not in git).

| | names | hash32 | hash64 |
|---|---|---|---|
| journal write, average of 4 shapes | 0 | −12.6% | +14.7% |
| track with 20 nested clips | 743 B | +33% | +88% |
| long-named settings struct | 404 B | −73% | −63% |
| file after 2000 inserts | 248 KB | +3.9% | +17.4% |
| encode/decode, small documents | | 25–35% faster | 25–35% faster |

**Costs.** Per-field hashes repeat for every element of a vector of structs. Files stop describing themselves: no field-name listing, no JSON conversion of an unknown file, `reflectPolymorphic` stops loading, and a missing field can silently match another field with the same hash. Protobuf avoids collisions by hand-numbering fields instead.
