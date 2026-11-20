# 0011 Miro: compute each type's name once, at compile time

**Status:** Accepted, 2026-11-15 (delegated). Jamie delegated these, 2026-11-15: "i want you to make good choices that prio: simple to build, ergonomic to use, extremely high perf and quick to load." PR: eyalamirmusic/Miro#29 (draft, head e9d84f2, CI 8/8 green, MSVC included). Measured on 1M {int, double} structs through JSON: to JSON 390 → 308 ns per object, from JSON 479 → 404 ns (Clang Release, alternating builds, load 6–7). `typeNameOf()`/`qualifiedNameOf()` keep their signatures and return `inline constexpr` variable templates; TypeName.h codehealth 8.65 → 9.18.

**Problem.** `reflectValue` builds `TypeId {typeNameOf<T>(), qualifiedNameOf<T>()}` at runtime for every object and array element, scanning `__PRETTY_FUNCTION__` each time: about 100 ns per object, ~2% of the 1 GB decode profile.

**Fix.** Make it `static constexpr`. Measured on a patched scratch copy: 75–145 ns saved per small document, ~2 µs on a track with 20 clips (see 0007).

**Compile time.** Not slower. Clean builds of Miro and its tests, alternating develop and the PR under the benchmark lock on macprorack: develop 276 s and 276 s of CPU, the PR 271 s and 269 s; wall time 51–54 s for both. The difference is within noise.

**No call site changes.** `typeNameOf<T>()` and `qualifiedNameOf<T>()` keep their signatures, so every existing caller gets the speed without edits.

