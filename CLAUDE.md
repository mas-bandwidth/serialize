# CLAUDE.md

## What this is

A single-header C++ bitpacking serializer (~2,100 lines of library code in
[serialize.h](serialize.h), plus ~700 lines of embedded tests) aimed at game
networking. It descends from the yojimbo/netcode lineage: a word-at-a-time
`BitWriter`/`BitReader` core, and `WriteStream`/`ReadStream`/`MeasureStream`
wrappers driven through templated `Serialize()` methods so one function handles
read, write, and measure with compile-time branch elimination.

Build: `cmake -B build && cmake --build build --config Release`, test with
`ctest --test-dir build --build-config Release`. Tests live in serialize.h
behind `SERIALIZE_ENABLE_TESTS`. CI (.github/workflows/ci.yml) builds and
tests Debug + Release on Linux (ubuntu-24.04), macOS Apple Silicon
(macos-15), and Windows x64 (windows-2025), plus ASan+UBSan and libFuzzer
jobs on Linux and a big-endian s390x job (GCC cross-compile, statically
linked, run under QEMU user emulation). The fuzz harness ([fuzz.cpp](fuzz.cpp), clang only, built via
`-DSERIALIZE_FUZZ=ON`) runs two passes per input: a hostile read of
arbitrary bytes through every ReadStream primitive, and a differential
write→read round trip that traps on any write/read asymmetry (and checks
MeasureStream never under-measures). A golden wire-format test
(`test_golden_wire_format` in serialize.h) pins the exact bytes the
serializer produces; if it fails, the wire format changed — a breaking
change for previously written data.

## Honest assessment

### Verified state (July 2026)

- All tests pass in both debug and release builds.
- All tests pass under ASan + UBSan (clang, `-fsanitize=address,undefined`).
- Compiles clean with `-Wall -Wextra -Wpedantic`. `-Wconversion -Wshadow`
  produces ~80 warnings — implicit narrowing is a deliberate style here
  (the header disables MSVC C4244 for the same reason).

### What's genuinely good

- **The read path is defensive, and recently hardened.** Every `ReadStream`
  operation bounds-checks before reading and range-checks after
  ([serialize.h:1056](serialize.h:1056)), returning false instead of asserting,
  so malicious packets fail cleanly. Arithmetic that could overflow signed
  ints is done in the unsigned domain with comments explaining why (e.g.
  [serialize.h:908](serialize.h:908), [serialize.h:1726](serialize.h:1726)),
  and NaN is clamped before any float-to-int cast
  ([serialize.h:1454](serialize.h:1454)). Recent commits show active work here.
- **The tests cover adversarial cases, not just round trips**: out-of-range
  encodings smuggled into bit headroom, full `[INT32_MIN, INT32_MAX]` ranges,
  negative and huge byte counts, NaN input, >2^31 relative gaps
  ([serialize.h:2610](serialize.h:2610) onward). This is better test thinking
  than most serialization libraries have.
- **The core design is sound and well understood.** 64-bit scratch, 32-bit
  flush, little-endian wire format with byte-swap on big-endian hosts; both
  the reader and the writer move words via `memcpy` (identical codegen to a
  direct store), so neither buffer needs any particular alignment.
  `WriteBytes`/`ReadBytes` fall back to `memcpy` for the middle of large
  blocks. `test_unaligned_writer` locks the no-alignment guarantee in.
- Documentation density is high, and the doc comments mostly tell you the
  sharp edges (flush requirement, 256 MB limit, alignment contracts).

### Sharp edges and weaknesses

- **No runtime bounds checking on the write path in release builds.**
  `BitWriter::WriteBits` ([serialize.h:405](serialize.h:405)) guards with
  asserts only; in release, writing past the end of the buffer is silent
  memory corruption. This is the intended trust model (writer is trusted,
  reader is not), but it's the single biggest way for a user to hurt
  themselves. Buffers must be sized conservatively or pre-measured with
  `MeasureStream`.
- **BitReader may read up to 3 bytes past a non-multiple-of-4 buffer.** The
  constructor doc says the allocation must round up to 4 bytes
  ([serialize.h:620](serialize.h:620)), but a caller who hands it an exactly
  sized buffer has an out-of-bounds read that ASan will flag. The contract is
  documented; it's still easy to miss.
- **The macro API is a loaded weapon.** `serialize_*` macros hide
  `return false` and require being called inside a `template <typename
  Stream>` function returning bool — documented, but surprising control flow.
  ~30 macros (`serialize_int`, `read_bits`, `write_object`, ...) land in the
  global macro namespace despite being defined inside `namespace serialize`;
  they will collide with yojimbo, which uses the same names.
- **Header hygiene leaks into consumers**: `#pragma warning(disable: 4127,
  4244)` with no push/pop ([serialize.h:112](serialize.h:112)) alters warning
  state for every MSVC translation unit that includes it, and the header pulls
  in a broad set of libc headers.
- Minor: `BitWriter()` zeroing itself via `memset(this, ...)`
  ([serialize.h:361](serialize.h:361)) is fragile if a non-trivial member is
  ever added; default-constructed streams have no guard against use before
  `Initialize()`; `test.cpp` seeds `srand` but nothing uses it; BUILDING.md
  still points at VS2019.

### Known limits (documented, by design)

- Max buffer 256 MB (bit counts held in signed 32-bit ints).
- `serialize_int_relative` requires strictly increasing values.
- `wstring` wire format is 32 bits per character — portable across 2/4-byte
  `wchar_t` platforms, but wasteful.
- `MeasureStream` is conservative: every align counts as 7 bits.

### Bottom line

Small, mature, and does one thing well. The reader-side safety work and the
adversarial tests are the standout strengths. The risks are concentrated in
the documented-but-sharp buffer contracts (unchecked writes in release, the
round-up-to-4 read contract). Those are inherent to the
design and documented; everything cheap to fix around them (CI, sanitizers,
fuzzing, doc drift) has been done. Fuzz coverage: a 60-second smoke on every
push, plus a nightly 1-hour run (.github/workflows/nightly-fuzz.yml) whose
corpus accumulates across runs via the actions cache and which uploads crash
reproducers as artifacts on failure.

## Roadmap

Done:

- ~~Golden wire-format test~~ — exact bytes pinned in serialize.h, checked
  write-side and read-side on every CI platform.
- ~~Differential round-trip fuzzing~~ — write/read asymmetry and
  MeasureStream conservatism are fuzzed alongside the hostile-read pass.
- ~~Writer alignment UB~~ — `BitWriter` now stores each dword with `memcpy`
  like the reader loads them (verified identical codegen), so writer
  buffers no longer need 4-byte alignment; `test_unaligned_writer` locks
  this in under the CI alignment sanitizer.

- ~~Benchmark target~~ — [bench.cpp](bench.cpp) measures the raw bitpacker
  and the stream/macro path (best-of-5 trials); CI prints indicative
  numbers on every Release job.

- ~~Big-endian CI coverage~~ — s390x jobs (Debug + Release) cross-compile
  with GCC and run the full test suite under QEMU, exercising the
  bswap/`host_to_network` path; the golden wire-format test proves the
  wire bytes are identical to the little-endian platforms.

Remaining:

1. **CMake consumer-friendliness.** FetchContent/add_subdirectory consumers
   currently inherit the test/example/fuzz targets and global compile
   flags. Guard those behind `PROJECT_IS_TOP_LEVEL`, scope flags to
   targets, add a `SERIALIZE_VERSION` define plus git tags.

Deliberately not doing: changing the reader's round-up-to-4 allocation
contract (owner decision: it is an intentional part of the design — do not
re-propose it); `-Wconversion`/`-Wshadow` cleanliness (high churn,
near-zero payoff given the deliberate implicit-conversion style);
modernizing the C++ (the C-ish style is a feature for this audience); or
any wire format change.
