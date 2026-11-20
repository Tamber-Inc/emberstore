# 0017 Calder files use the magic CLD1 / CLDJ and the extension .calder

**Status:** Accepted, 2026-11-16. Jamie: "load result. mit. approve extension change." In progress, with 0016.

**Problem.** The format is called Calder (0015), but its files start with `MIB1` (documents) and `MIBJ` (journals), and the docs disagree on the extension (`.mib`, `.ember`, `.calder`).

**Decision.** Documents start with `CLD1`, journals with `CLDJ`, and Calder files end in `.calder`. Nothing has shipped, so readers don't accept the old magics. SPEC.md and the Python decoder change in the same commit as the C++. physicsgame2's levels become `.calder`.

**Same message.** `LoadReport` becomes `LoadResult` (0016), and Calder is licensed MIT (0015).
