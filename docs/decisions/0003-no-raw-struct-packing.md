# 0003 Don't pack arrays of structs as raw bytes because they are trivially copyable

**Status:** Superseded by 0009, 2026-11-15. (Was: Deferred, 2026-11-14; Jamie chose number types only "for now" in 0002.)

**Problem.** Packing `std::vector<Vec3>` or any trivially copyable struct as one block would be fast.

**Why not now.** Trivially copyable means safe to copy in memory, not safe to store:
- Padding bytes are uninitialised. A disengaged `std::optional<float>` writes 7 of its 8 bytes from leftover memory (measured on macprorack with g++ and clang++, libstdc++).
- `std::string_view`, `std::span` and raw pointers are trivially copyable: their addresses would go to disk.
- Standard-library layouts aren't promised to match across libstdc++, libc++ and MSVC.
- A struct stored as one blob has no field names, so adding a field breaks every old file.
- `std::has_unique_object_representations_v` rejects padding but also rejects `float`, `double` and every float struct.

**If revisited.** A per-type opt-in for padding-free structs of fixed-width numbers, with the struct's field layout recorded in the file.
