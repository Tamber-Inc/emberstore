# 0010 Decode large arrays in parallel

**Status:** Deferred, 2026-11-15. Jamie: "i'm also super interested in single threaded decode. let's try and do multi-threaded last resort." The experiment was stopped and its partial results are recorded below.

**Problem.** Decoding is single-threaded: 1 GB of game state takes 9.3 s on one of macprorack's 16 cores.

**Idea.** Every container stores its byte size up front, so a large array (the entity list) can be split into chunks and decoded on many threads. Finding chunk boundaries costs a skip-scan, or a small element-offset index written with big arrays (a format change, to be measured both ways).

**Partial results (stopped early).** macprorack, clang 21 Release, decode only, medians of 3, configurations alternated. The machine was busy with other agents' niced builds for the larger sizes, so treat those as rough.

| Size | Load | 1 thread | 2 | 4 | 8 | 16 |
|---|---|---|---|---|---|---|
| 100 MB | 2–4 | 0.90 s | 0.49 s | 0.29 s | 0.18 s | 0.17 s |
| 1 GB | ~8 | 9.4 s | – | – | – | 1.6 s |
| 2 GB | 10–35 | 17.1 s | 9.4 s | 7.6 s | 5.0 s | 4.3 s |

Decoded worlds equal the originals at every size. The hook costs 0.5–2% on one thread. Scaling past 8 threads is limited mostly by glibc malloc contention (2% → 11% of samples). Design: an optional Miro hook `loadArrayElements(elementCount, childOpts, loadElement)` (default false, like `visitPacked`); emberstore takes a `ParallelLoadPolicy {threadCount = 1, ...}` as a last argument, so the default is today's behaviour. No format change. ThreadSanitizer clean. Code: `~/projects/Miro-jp-parallel-load` (commit 8747da8, based on jp/reflector-hooks, not develop) and `~/projects/emberstore-parallel-decode`. Rerun: `Apps/GameStateBench/run-parallel.sh` in the copy.
