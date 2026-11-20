# 0007 Cut emberstore's per-field reflection cost

**Status:** Accepted, 2026-11-15 (delegated). Jamie delegated these, 2026-11-15: "i want you to make good choices that prio: simple to build, ergonomic to use, extremely high perf and quick to load." Landed in this tree (uncommitted): the five `emberstore/Binary` files copied unchanged from the experiment copy; 158/158 tests.

**Problem.** emberstore encodes and decodes small documents 5–25× slower than protobuf. A profile puts the time in reflection plumbing, not the format: a `new` child reflector per field, a hashed string lookup per field name, `qualifiedNameOf<T>` per array element, and an `Options` object built per child.

**Evidence.** macprorack, clang 21, `-O3`, protobuf v32.1 built from source with the same flags, medians of 21 batches. "pb+copy" includes copying a plain struct into the protobuf message and back. Script: `~/projects/emberstore-protobuf-bench/Apps/ProtobufBench/run.sh` (copy, not in git).

| shape | emberstore bytes | protobuf bytes | emberstore enc / dec | pb+copy enc / dec |
|---|---|---|---|---|
| Note | 138 | 88 | 986 / 843 ns | 225 / 267 ns |
| 6 small fields | 52 | 22 | 1015 / 903 ns | 47 / 47 ns |
| 10 long-named fields | 385 | 48 | 1816 / 1454 ns | 103 / 125 ns |
| track with 20 clips | 724 | 628 | 12.0 / 11.0 µs | 1.7 / 2.0 µs |
| 1M floats | 4.0 MB | 4.0 MB | 597 / 455 µs | 748 / 587 µs |

**Proposal.** Reuse child reflectors instead of allocating one per field, and look keys up without a per-field string hash. Measure against this table.

**Result.** Same machine, clang 21, -O3; median of 3 alternating rounds of 21 batches; load 1.1–1.4 on 16 cores. Script: `~/projects/emberstore-reflector-speed/Apps/ProtobufBench/compare-reflector-speed.sh`.

| shape | before enc / dec | after enc / dec | pb+copy enc / dec |
|---|---|---|---|
| Note | 871 / 810 ns | 555 / 563 ns | 216 / 256 ns |
| 6 small fields | 998 / 846 ns | 548 / 499 ns | 48 / 49 ns |
| 10 long-named fields | 1910 / 1358 ns | 790 / 702 ns | 115 / 130 ns |
| track with 20 clips | 11.1 / 10.2 µs | 6.2 / 7.7 µs | 1.6 / 2.0 µs |
| 1M floats | 601 / 438 µs | 594 / 432 µs | 742 / 591 µs |

Changes: one reused child reflector per nesting depth instead of one allocation per field; an interned key table that predicts the next key; the decoder's unused per-load key hash map deleted; Options moved instead of copied. Output bytes identical to before for every shape plus a coverage document; 158/158 tests, 157/157 under AddressSanitizer. Reviewed: child reuse is safe against current Miro (only `ReflectTagged.h` holds a child, and it is done before the next request).

**Miro follow-ups (measured on a patched scratch copy, not proposed yet).** `qualifiedNameOf<T>()` is computed at runtime per object and per array element (~100 ns each); making the TypeId `static constexpr` saves 75–145 ns per small document. Copying `CustomOptions`' `std::string` into every child costs another 25–110 ns.

