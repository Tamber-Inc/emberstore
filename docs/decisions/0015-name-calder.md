# 0015 The format is called Calder

**Status:** Accepted, 2026-11-15. Jamie: "calder for now. sounds good. jamierpond/Calder please".

**Why the name.** Alexander Calder invented the mobile: nested, balanced shapes hanging from one another, each branch self-contained, which is what a reflected object tree in this format is. He was Joan Miró's close friend for nearly 50 years, so it sits beside Miro. No well-known serialization format uses the name.

**Where it lives.** https://github.com/jamierpond/Calder (public): `SPEC.md` (moved unchanged from `docs/binary-format-spec.md` here, apart from its title) and `python/mib`, the decoder written from the spec alone (moved from `tools/python/mib`). The C++ encoder and decoder moved there on 2026-11-16: copied unchanged in Calder `914a8b9`, then renamed in `2f14d10` and `6c9db53` (namespace `emberstore::Binary` and the `emberstore::toBinary` family become `Calder::`, included as `<Calder/Calder.h>`, library target `calder`; the reflectors `BinaryWriterReflector` / `BinaryReaderReflector` become `Calder::Writer` / `Calder::Reader`, and the byte writer that was `Writer` becomes `Calder::Detail::ByteWriter`). emberstore no longer has `emberstore/Binary/` or `tests/Binary/`; it fetches Calder by CPM, pinned to a commit sha, and its own code, apps and tests use the `Calder::` names. The format's tests (94) run in Calder.

**Not done yet.** Files still start with `MIB1` / `MIBJ`; changing the magic to follow the name is a format change for later. Packed struct arrays (0009) update the spec and decoder in their experiment copy; if accepted, those changes go to Calder.

**License.** MIT (Jamie, 2026-11-16: "mit").

**Update, 2026-11-16.** The files now carry the name too (0017): magic `CLD1` / `CLDJ`, extension `.calder`, Python decoder `python/calder`. Calder is MIT licensed.
