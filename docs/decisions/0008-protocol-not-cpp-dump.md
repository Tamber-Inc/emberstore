# 0008 The binary format is a protocol, readable without the C++ code

**Status:** Accepted, 2026-11-15. Jamie: "Yeah I think this makes sense ... we should start on a python impl in the background to show it's _just a protocol_." Keep building in emberstore for now, as the stress test; a separate repo (jamierpond/<name>) may follow.

**Problem.** If decoding a file needs knowledge of the C++ structs or Miro internals, nobody can read the data from another language, and the format is really a C++ memory dump with extra steps.

**The test.** "How annoying would a Go decoder be?" Someone with only the written spec must be able to decode any file into a generic tree with JSON's shapes (named fields, arrays, strings, numbers, booleans, null), without the C++ types.

**What it takes.**
1. A written spec of the format with no C++ in it.
2. A second decoder in another language (Python first), written only from the spec.
3. A test that decodes every test file with both and checks the JSON matches.

**Known leaks of C++ into today's format.**
- A `uint64_t` above 2^63 is stored as its int64 bit pattern; a generic decoder sees a negative number.
- A `std::variant` is stored as an object whose one field is named after the C++ type (`"int"`, `"vector<float>"`).

**Known limit.** Container sizes are 32-bit, so one document can't exceed 4 GiB.
