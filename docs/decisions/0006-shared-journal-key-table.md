# 0006 One key table shared by the whole journal

**Status:** Accepted, 2026-11-15 (delegated), as the only journal format, replacing today's: one format is simpler than two, and nothing has shipped. Jamie delegated these, 2026-11-15: "i want you to make good choices that prio: simple to build, ergonomic to use, extremely high perf and quick to load." Landed in this tree (uncommitted), 2026-11-15: the per-frame key table journal is gone; `binaryJournalSet(key, value, journalKeyTable)` returns `std::optional<Buffer>` (nullopt when the value names a field the base lacks), `Binary::readJournalKeyTable(journal)` gives the table, and `Document` compacts instead of appending in that case. `Database` / `Document` / `Collection` APIs unchanged. The magic stays "MIBJ": the header and frame envelope are unchanged, only a set frame's value lost its own header and table, and no file has shipped. 162 C++ tests and 50 Python decoder tests pass. Sqlitestore Note: 155 → 123 bytes per journal write (−20.6%, measured 2026-11-15 on macprorack); insert timings unchanged.

**Problem.** Each journal write is a complete document with its own key table, so every write repeats all its field names.

**Design.** The journal's key table is the compacted base's table; writes refer to names by 1-byte ids. Single documents are unchanged. (The experiment used a separate "MIBS" journal type beside today's; the port replaced today's instead and kept "MIBJ".)

**Revision (Jamie, 2026-11-14: "sure").** A write never adds a name. When a write would use a field name the table doesn't have, the store compacts instead, writing a fresh base that includes it. Then every write depends only on the base, and a corrupt write in the middle loses only itself, as today. The first version let writes add names, so a corrupt write could take later writes down with it.

**Evidence (first version).** macprorack, Release, ext4, medians of 3. Script: `~/projects/emberstore-key-hashing/Apps/Sqlitestore/bench-key-hashing.sh` (copy, not in git).

| shape | names frame | shared table |
|---|---|---|
| Note | 157 B | −19% |
| 6 short fields | 71 B | −28% |
| 10 long-named fields | 404 B | −81% |
| track with 20 clips | 743 B | −9% |
| file after 2000 inserts | 248 KB | −6.9% |
| file after 500 durable inserts | 65 KB | −12.8% |

Average write 34% smaller, never bigger than names; insert speed unchanged.

**Evidence (revised: a write never adds a name).** Same box, disk and script; results in `bench-results-compact-on-new-name/`. 170 tests pass.

| shape | names frame | shared table |
|---|---|---|
| Note | 157 B | −20.4% |
| 6 short fields | 71 B | −31.2% |
| 10 long-named fields | 404 B | −81.4% |
| track with 20 clips | 743 B | −8.9% |
| file after 2000 inserts | 248 KB | −7.5% |
| file after 500 durable inserts | 65 KB | −13.1% |

Average write 35% smaller, never bigger than names; insert speed and load time unchanged. A corrupt write in the middle now loses only itself (tested). A struct that gains a field compacts once on its first write with the new name, then appends again (tested). The bench never adds a field, so the rule caused 0 extra compactions there.

**Port notes.** Not ported: the hash32/hash64 key modes (0005), the key-mode parameter, the "names" journal kept beside the shared one, and the experiment's bench code and teaching page. A set frame is skipped when its value's first tag is unknown or the base's key table is unreadable; an unknown key id inside a frame reads as a missing field, like everywhere else in the C++ reader. The format spec ([Calder's `SPEC.md`](https://github.com/jamierpond/Calder/blob/main/SPEC.md) section 6, example 11.5) and the Python decoder ([Calder's `python/mib`](https://github.com/jamierpond/Calder/tree/main/python/mib)) describe and decode the new layout.
