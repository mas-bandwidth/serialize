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

- All tests pass in Debug and Release on Linux x64, macOS Apple Silicon,
  Windows x64, and big-endian s390x (GCC cross-compile under QEMU), on
  every push.
- The golden wire-format test proves all four platforms — including big
  endian — produce and decode byte-identical wire data.
- All tests pass under ASan + UBSan including the alignment sanitizer.
- Fuzzing (hostile read + differential write→read round trip): 60 seconds
  per push, 1 hour nightly with a cumulative corpus. No findings to date.
- Compiles clean with `-Wall -Wextra -Wpedantic`. `-Wconversion -Wshadow`
  produces ~80 warnings — implicit narrowing is a deliberate style here
  (the header disables MSVC C4244 for the same reason).
- Header and CMake version is 1.3.0 (`SERIALIZE_VERSION`); the last pushed
  tag is v1.2.5.
- Throughput ([bench.cpp](bench.cpp), Release, Apple Silicon reference):
  bitpacker write ~4.6 GB/s, read ~2.1 GB/s; stream write ~25M packets/s,
  read ~43M packets/s.

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
  `Initialize()`; `test.cpp` seeds `srand` but nothing uses it.

### Known limits (documented, by design)

- **The trust model**: debug asserts verify correctness, and in release
  correctness is the caller's responsibility — there is no runtime bounds
  checking on the write path (size buffers conservatively or pre-measure
  with `MeasureStream`). The one exception is network input: the read path
  validates at runtime in release and drops invalid data, because asserts
  are not enough at the trust boundary. Do not propose hardened/checked
  write modes.
- Max buffer 256 MB (bit counts held in signed 32-bit ints).
- `serialize_int_relative` requires strictly increasing values.
- `wstring` wire format is 32 bits per character — portable across 2/4-byte
  `wchar_t` platforms, but wasteful.
- `MeasureStream` is conservative: every align counts as 7 bits.

### Bottom line

Small, mature, and does one thing well. The reader-side safety work and the
adversarial tests are the standout strengths. The buffer contracts
(unchecked writes in release, the round-up-to-4 read contract) are
intentional design — debug asserts plus caller responsibility — and the
place for a new user to read the docs carefully; everything cheap to fix
around them (CI, sanitizers, fuzzing, doc drift) has been done. Fuzz coverage: a 60-second smoke on every
push, plus a nightly 1-hour run (.github/workflows/nightly-fuzz.yml) whose
corpus accumulates across runs via the actions cache and which uploads crash
reproducers as artifacts on failure.

### Open items

- **The v1.3.0 tag is not pushed.** The header and CMake already say 1.3.0
  (next after v1.2.5, covering the CMake switch, the
  `serialize_ack_relative_internal` removal, and the writer alignment
  guarantee). Cutting the tag/release is the owner's call.
- ~~GCC stream benchmark numbers are inflated~~ — fixed, in two parts:
  a `bench_escape` barrier (empty asm + memory clobber) stops dead-store
  elimination of the output buffer, and an LCG varies most packet fields
  per iteration so GCC can no longer constant-fold the loop-invariant
  fields' scratch words at compile time. GCC still reports notably higher
  stream numbers than MSVC (~92M vs ~33M packets/s) — that residual gap is
  legitimate codegen (static field offsets merge adjacent writes), not
  elimination.
