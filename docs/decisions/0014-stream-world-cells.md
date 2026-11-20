# 0014 Stream one world cell without decoding the whole store

**Status:** Measured, 2026-11-16. Recommendation: accept the design below. Waiting for Jamie.

**Problem.** A game streams a world one cell at a time. Today `Collection::get` decodes every document when the collection opens: 4.4 s for a 1M-entity world in 64 m cells, to read one cell of 53 entities.

**Evidence.** Decoding one cell straight from the file (memory-mapped, other cells untouched) took 0.1–0.26 ms up to 100k entities and 3.8 ms at 1M (52,074 cells); the 3.8 ms is mostly scanning keys to find the cell. Script: `~/projects/physicsgame2/scripts/scene-load-benchmark.sh`; results `Levels/Generated/results.txt`.

**What it needs.**
1. A public way to decode one value from a key inside a file (today the experiment calls the internal `BinaryReaderReflector`).
2. A keyed index for a map's entries, so finding one cell doesn't scan every key.
3. A `Collection` that reads one document on demand instead of decoding all of them on open.

**Result of the exploration.** Big maps (64+ entries) get a sorted key index after their entries, so finding a cell is a binary search; the store maps the file and decodes only the cell asked for. macprorack, Release, warm cache, medians:

| entities (cells) | today: open + get one cell | heap | indexed, mapped: open + get | heap |
|---|---|---|---|---|
| 1k (52) | 6.0 ms | 2.3 MB | 0.32 ms | 1.2 MB |
| 10k (526) | 42.6 ms | 12.6 MB | 0.06 ms | 1.2 MB |
| 100k (5,186) | 434 ms | 107 MB | 0.19 ms | 1.3 MB |
| 1M (52,074) | 4,520 ms | 459 MB | 0.06 ms | 1.3 MB |

Each further cell costs 70–130 µs, which is decoding the cell itself. The index costs 4 bytes per entry (+0.07% at 1M). A decoder that wants the whole tree reads an indexed map like a plain map and ignores the index; the Python decoder needed ~60 lines. Compaction can copy each live value's bytes without decoding (0.72 s vs 8.2 s at 1M).

**Design.** A new tag `IndexedMap` (entries as in Map, then `u32 entryOffset[count]` sorted by key bytes, then `u32 count`); `File::open` maps the file, `createFromBinary<T>(view)` decodes one slice; a binary `Collection` that opens without decoding and reads one document on demand, same API as today. Before landing: cap the journal tail read on open, and stream compaction to the file instead of building it in memory (peaked at 0.9 GB at 1M). POSIX mmap only in the prototype; no inter-process lock yet.

**Clash to resolve when landing.** The prototype numbered `IndexedMap` tag 18, which 0009 already uses for `PackedStructArray`; it takes the next free tag.

Code and scripts: `~/projects/emberstore-stream-cells`, `~/projects/physicsgame2-stream-cells/scripts/stream-cells-benchmark.sh`.

