# Decisions

This folder records every choice made about emberstore's binary format and
the Miro changes it depends on: what problem it solved, what was tried, what
it measured, and who decided.

## How the work is organised

The goal is a binary format for apps and games that is simple to build, easy
to use, very fast, and quick to load. Two rules follow from that:

- **Make things faster without changing how callers write code.** A user's
  `MIRO_REFLECT` struct and their `toBinary` / `createFromBinary` calls stay
  the same; the speed comes from inside.
- **The format is a protocol, not a C++ memory dump.** Anyone with the spec
  ([Calder's `SPEC.md`](https://github.com/jamierpond/Calder/blob/main/SPEC.md)) can read a file in another language; the
  Python decoder in [Calder's `python/calder`](https://github.com/jamierpond/Calder/tree/main/python/calder) proves it, and Calder's CI checks it reads every sample file the way the C++ does.

Work lives in three places:

| Place | What goes there | Who reviews it |
|---|---|---|
| Miro pull requests on `eyalamirmusic/Miro` | general reflection changes any format can use | Eyal |
| this emberstore tree | the binary format, the store, the spec, this log | Jamie |
| experiment copies, `~/projects/emberstore-*` | throwaway trials, each with its benchmark script | nobody; they are measured, then kept or dropped |

Each idea moves through the same steps:

1. **Proposed:** the idea and why it might help.
2. **Measured:** it was tried in an experiment copy; the file has the numbers.
3. **Accepted, Rejected or Deferred:** Jamie decides, or delegates the
   decision; the file quotes his words.
4. **Landed:** the code is in this tree or in a Miro PR, and the file links it.

## Rules

- A decision is **Accepted**, **Rejected** or **Deferred** only when Jamie says so. The file
  quotes his words and the date. Until then it is **Proposed** (an idea) or
  **Measured** (an experiment ran and the numbers are in the file), with a
  recommendation.
- Every number names the machine, the build and the script that produced it,
  so anyone can rerun it.
- An accepted decision that changes code points at the commit or PR that
  carries it. When the code lands, the file's status says so.
- A decision is never edited away. If it is reversed, the new file says which
  one it replaces, and the old one's status becomes **Superseded by NNNN**.
- Experiments live in throwaway copies (`~/projects/emberstore-*`). They are
  not backed up. When an experiment is accepted, its code and benchmark script
  move into this repo.

## Index

| # | Decision | Status |
|---|---|---|
| [0001](0001-no-legacy-keys.md) | Renamed fields are migrated in the struct's own `reflect()`, not by a Miro `legacy()` key | Accepted |
| [0002](0002-visit-packed-in-miro.md) | Miro hands a whole array of fixed-width numbers to the reflector (`visitPacked`) | Accepted, PR open |
| [0003](0003-no-raw-struct-packing.md) | Don't pack arrays of structs as raw bytes because they are trivially copyable | Superseded by 0009 |
| [0004](0004-loading-fixes-in-miro.md) | Missing lists/maps keep their defaults; numbers that don't fit are refused | Accepted, PRs #27 and #28 open |
| [0005](0005-no-key-hashing.md) | Store field names as 32/64-bit hashes | Rejected |
| [0006](0006-shared-journal-key-table.md) | One key table shared by the whole journal | Accepted (delegated), landed (uncommitted) |
| [0007](0007-reflector-per-field-cost.md) | Cut emberstore's per-field reflection cost (5–25× slower than protobuf on small documents) | Accepted (delegated), landed |
| [0008](0008-protocol-not-cpp-dump.md) | The binary format is a protocol, readable without the C++ code | Accepted, spec and Python decoder in progress |
| [0009](0009-pack-simple-structs.md) | Pack arrays of simple reflected structs (`vector<Vec3>`) | Accepted, landed in Calder; Miro PR #32 open |
| [0010](0010-parallel-decode.md) | Decode large arrays in parallel | Deferred: single-threaded first |
| [0011](0011-compile-time-type-names.md) | Miro: compute each type's name once, at compile time | Accepted (delegated), PR #29 open, CI 8/8 |
| [0012](0012-documents-over-4gib.md) | A document over 4 GiB silently decodes as empty | Accepted (delegated): writer refuses, landed |
| [0013](0013-pack-external-structs.md) | Pack number-only structs reflected with `MIRO_REFLECT_EXTERNAL` | Accepted, landed in Calder's Miro pin; Miro PR #32 open |
| [0014](0014-stream-world-cells.md) | Stream one world cell without decoding the whole store | Measured, recommend accept |
| [0015](0015-name-calder.md) | The format is called Calder | Accepted; the C++ code lives in Calder |
| [0016](0016-calder-public-api.md) | Calder's public API: fun to use, hard to misuse | Accepted, landed in Calder 98bbd57 |
| [0017](0017-calder-magic-and-extension.md) | Calder files use the magic CLD1 / CLDJ and the extension .calder | Accepted, in progress |
