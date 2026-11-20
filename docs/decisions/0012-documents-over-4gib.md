# 0012 A document over 4 GiB silently decodes as empty

**Status:** Accepted, 2026-11-15 (delegated): option 1, the writer refuses. 64-bit sizes would add 4 bytes to every container for a case chunking handles better (chunks peaked at 4.5 GB of memory against 20.6 GB, and suit parallel loading). Jamie delegated these, 2026-11-15: "i want you to make good choices that prio: simple to build, ergonomic to use, extremely high perf and quick to load." Landed in this tree (uncommitted): `Binary::Detail::convertToStoredSize` throws `std::length_error` for a document, journal base or frame over 4 GiB; test `Binary: a document over 4 GiB is refused, not written`.

**Problem.** `toBinary` on a 5.1 GB world returns a 5.1 GB buffer with no error, and `createFromBinary` on it returns an empty world (0 entities) with no error and no load report. Container sizes and the key-table offset are 32-bit, so they wrap. The data is lost silently, on write.

**Options.**
1. The writer refuses (throws) when any size or offset would exceed 32 bits; callers split their data (the benchmark's chunked mode shows 1 GB chunks work: 10.2 GB in 11 documents decodes correctly).
2. 64-bit sizes and offsets (a format change: +4 bytes per container), or a per-document flag for wide sizes.

**Evidence.** macprorack, Release: `~/projects/emberstore-game-state-bench/Apps/GameStateBench/results/binary-5000.txt` (encoded_bytes=5104456716, decoded_entities=0).

**Revisited 2026-11-15: stay 32-bit.** Jamie: "ok then 32bit probs fine for now until our users start complaining." Real single files (an editor level) top out around 0.1–1 GB (see `../research/2026-11-15-realistic-game-data.md`); bigger worlds are streamed cells (0014). If a real case needs it: a `MIBW` magic meaning 64-bit sizes, counts and key-table offset; one `readStoredSize()` used by the ~6 read sites; the writer encodes in 32-bit and re-encodes in 64-bit only when a size overflows. Estimated 60–100 lines plus tests, a sentence in the spec, ~5 lines in the Python decoder. Practical ceiling would then be memory (a 5 GB document peaked at 20.6 GB RAM), not the format.
