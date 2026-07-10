# CLAUDE.md

## What this is

A single-header C++ bitpacking serializer (~2,100 lines of library code in
[serialize.h](serialize.h), plus ~700 lines of embedded tests) aimed at game
networking. Header-only is intentional: the serialize methods are heavily
templated, so the implementation cannot live in a .cpp file. The header is
self-contained — including it into a translation unit with no prior
includes must compile — and includes only the libc headers the library
uses (stdint, stddef, string, wchar, math, plus conditional assert/endian;
test-only includes live behind `SERIALIZE_ENABLE_TESTS`). It descends from the yojimbo/netcode lineage: a word-at-a-time
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
- Header and CMake version is 1.4.0 (`SERIALIZE_VERSION`), matching the
  v1.4.0 tag and GitHub release (latest, July 2026). 1.4.0 carries the branchless
  reader and its breaking allocation contract change (read buffers must
  extend 8 bytes past the data, previously round-up-to-4), plus 64-bit bit
  counts throughout, which removes the old 256 MB buffer limit
  (test_large_buffer round trips across the old 2^31-bit boundary); the
  wire format is unchanged.
- Throughput ([bench.cpp](bench.cpp), Release, Apple Silicon reference):
  bitpacker write ~4.6 GB/s, read ~8.1 GB/s; stream write ~25M packets/s,
  read ~142M packets/s. (Reads got ~4x faster in 1.4.0 with the branchless
  reader.)

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
- **The core design is sound and well understood.** Writer: 64-bit scratch,
  32-bit flush, each word stored via `memcpy` (identical codegen to a direct
  store) so the buffer needs no particular alignment. Reader: branchless —
  each read loads a 64-bit window at the current byte position and shifts by
  the bit remainder, carrying no state between reads except the bit index.
  This made reads ~4x faster than the previous word-at-a-time reader
  (measured; see throughput above) at the cost of the 8-bytes-past
  allocation contract below. Little-endian wire format with byte-swap on
  big-endian hosts; identical wire bytes to the old reader/writer, pinned by
  the golden test. `test_unaligned_writer` locks the no-alignment guarantee
  in.
- Documentation density is high, and the doc comments mostly tell you the
  sharp edges (flush requirement, 256 MB limit, alignment contracts).

### Sharp edges and weaknesses

Nothing currently open. Items formerly listed here were either fixed or
confirmed as intentional design:

- Fixed: the MSVC `#pragma warning(disable: 4127, 4244)` is now push/pop'd
  so warning state no longer leaks into consumers (code using the
  serialize macros compiles at the including file's warning state;
  consumers who share the implicit-narrowing style disable those warnings
  themselves, as this repo's own executables do). `BitWriter` uses member
  initializers rather than `memset(this, ...)`. Using a stream before
  `Initialize()` fires an explicit debug assert. The header includes only
  the libc headers the library actually uses, and consumers can no longer
  accidentally depend on it providing stdio/stdlib.
- Intentional design (recorded under "Known limits" below): unchecked
  writes in release, the read allocation contract, the macro control
  flow.

### Known limits (documented, by design)

- **The trust model**: debug asserts verify correctness, and in release
  correctness is the caller's responsibility — there is no runtime bounds
  checking on the write path (size buffers conservatively or pre-measure
  with `MeasureStream`). The one exception is network input: the read path
  validates at runtime in release and drops invalid data, because asserts
  are not enough at the trust boundary. Do not propose hardened/checked
  write modes.
- **The serialize macros hide `return false` on purpose.** When reading a
  packet, invalid data must abort the entire serialize function
  immediately — never carrying on deeper into the serialization or into a
  loop bounded by malicious data. The library is low-level C-style and
  chooses not to use exceptions, so early-return macros are the pragmatic
  mechanism. Serialize functions must be `template <typename Stream>`
  returning bool (documented). Do not propose redesigns (exceptions, error
  codes). The ~30 macros land in the global macro namespace; not a
  collision risk for yojimbo, which depends on serialize.h directly.
- **BitReader loads 64-bit windows at byte granularity, so the buffer
  allocation must extend at least 8 bytes past the end of the packet
  data.** (Owner-approved contract change, July 2026: previously
  round-up-to-4.) This is what makes the branchless reader possible — the
  bytes past the end are loaded but never interpreted. Documented on the
  constructor; read it when allocating receive buffers. Do not propose
  removing this contract or adding tail branches to avoid the over-read.
- `serialize_int_relative` requires strictly increasing values.
- `wstring` wire format is 32 bits per character — portable across 2/4-byte
  `wchar_t` platforms, but wasteful.
- `MeasureStream` is conservative: every align counts as 7 bits.

### Bottom line

Small, mature, and does one thing well. The reader-side safety work and the
adversarial tests are the standout strengths. The contracts (unchecked
writes in release, the 8-bytes-past read allocation contract, early-return serialize
macros) are intentional design — debug asserts plus caller responsibility
on the trusted side, immediate validated abort on the network side — and
the place for a new user to read the docs carefully; everything cheap to
fix around them (CI, sanitizers, fuzzing, doc drift) has been done. Fuzz coverage: a 60-second smoke on every
push, plus a nightly 1-hour run (.github/workflows/nightly-fuzz.yml) whose
corpus accumulates across runs via the actions cache and which uploads crash
reproducers as artifacts on failure.

### Open items

- ~~The v1.3.0 tag is not pushed~~ — released July 2026: tag v1.3.0,
  GitHub release "Stable Release" marked latest, covering everything
  since v1.2.5 (CMake switch, CI/sanitizers/fuzzing/golden wire test,
  writer alignment guarantee, `serialize_int64`, header hygiene).
- ~~GCC stream benchmark numbers are inflated~~ — fixed, in two parts:
  a `bench_escape` barrier (empty asm + memory clobber) stops dead-store
  elimination of the output buffer, and an LCG varies most packet fields
  per iteration so GCC can no longer constant-fold the loop-invariant
  fields' scratch words at compile time. GCC still reports notably higher
  stream numbers than MSVC (~92M vs ~33M packets/s) — that residual gap is
  legitimate codegen (static field offsets merge adjacent writes), not
  elimination.
