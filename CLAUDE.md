<!-- HOT:BEGIN -->
## HOT — read before reasoning about this repo

WHAT: the C++ bitpacking serialization library. NOT serialize.rs / serialize.go (the ports),
NOT serialize.modern (the C++23 rewrite with compile-time schemas).

DECISIONS THAT READ AS BUGS (they are not — do not "fix" them)
- **The serialize macros hide `return false` on purpose.** Invalid data must abort the
  whole serialize function immediately, never continuing deeper or into a loop bounded by
  attacker-controlled data. The library is low-level C-style and deliberately does not use
  exceptions. Serialize functions must be `template <typename Stream>` returning bool. Do
  not propose exceptions or error codes; the early-return macro IS the mechanism.
- **~80 implicit-narrowing warnings are a deliberate style**, not neglect.
- **Asserts on the write path, runtime validation on the read path.** Write-side misuse is
  a programmer error (size buffers conservatively or pre-measure with `MeasureStream`); the
  READ path validates in release and drops invalid data, because asserts are not enough at
  a trust boundary. Do not propose hardened/checked write modes.
- **The buffer contracts are load-bearing** (owner-approved July 2026): write buffers must
  be a multiple of 8 bytes (the writer flushes qwords; bytes past the data are written only
  as zeros), and read allocations must extend at least 8 bytes past the packet (the reader
  loads 64-bit windows at byte granularity; bytes past the end are loaded but never
  interpreted). These contracts are what make the qword-flush writer and the branchless
  reader possible. Do not remove them or add tail handling to avoid them.
- `serialize_int_relative` requires strictly increasing values over the domain 0 to 2^31 - 1,
  for `previous` as well as `current`, and there are no wrap semantics. A `previous` outside the
  domain is caller error (debug asserted); a `current` off the wire outside it is refused, in
  every tier, reconstructed in a width that cannot wrap.
- **A refused read leaves a scalar destination unwritten, and fails the stream for good.** The
  first refusal poisons `BitReader`'s position past the end, so the past-end check every read
  already performs refuses every later read — the latch costs the read path nothing. `Initialize`
  clears it. Refusals decided outside the stream route through `serialize::serialize_fail`.
- `wstring` is 32 bits per character on the wire, for portability across 2/4-byte platforms.
- **The `__restrict`-qualified `this` on BitWriter::WriteBits/WriteBytes/FlushBits is a
  measured optimization (writes up to +152% in generated code), not decoration** — and the
  spelling matters: LLVM DROPS restrict on data members (a member `__restrict` is a silent
  no-op, byte-identical objects), only restrict-qualified member functions work. The
  contract (buffer must not overlap the writer object) is documented and debug-asserted;
  sanitizers cannot catch restrict violations. Upstream clang forbids a `__restrict` member
  function calling non-restrict members — spell asserts directly instead of calling
  helpers there. Do not remove the qualifiers, and do not trust member-restrict placements.

THE WRITE/READ RULE — this library is the clearest statement of it, IN ITS OWN DOCS
Glenn, 2026-07-26: "intention is on write, user is responsible to not crash or do undefined
behavior. asserts are there to help. callers responsibility. on read, obviously, we must
check." Plus Postel: "be conservative in what you send, permissive in what you receive."
serialize.h says it directly at :1477, :1525 and :1540 -- "All checking is performed by debug
asserts on write." That is the CONTRACT. I audited this header, quoted that exact line in my
notes, and still filed the write path as a defect. Do not repeat that.
DELIBERATELY ASSERT-ONLY ON WRITE, do NOT "fix":
  - BitWriter::WriteBits (:1034) and WriteBytes (:1091) -- the asserts are the whole bound.
  - BitWriter::Initialize / ctor (:994, :1013): serialize_assert( ( bytes % 8 ) == 0 ) is the
    ENTIRE enforcement of the qword-store contract that FlushBits (:1137) relies on. Hand a
    WriteStream a 100-byte buffer, write exactly 800 bits -- within capacity, violating no
    assert even in debug -- and the flush memcpys 8 bytes at offset 96, four PAST the end.
    Proved with a canary, 2026-07-26. Still the caller's responsibility: pass a multiple of 8.
    (yojimbo satisfies it on purpose at yojimbo_connection.cpp:248, `maxPacketBytes &= ~7`.)
  - serialize_copy_string / serialize_copy_wstring (:3172, :3186) with dest_size 0 -- the
    same size_t underflow as reliable_copy_string.
NOTE FOR ANY SANITIZER WORK HERE: ASan does NOT report that FlushBits overflow. It is a
partial-granule write (8 bytes at offset 96 of a 100-byte allocation) and ASan is blind to
it -- verified with a control, an identical raw memcpy is also unreported. "No ASan report"
is NOT evidence of safety in this header. Use a canary region.
THE READ PATH IS CLEAN and both independent audits agree: every BitReader assert has a real
ReadStream companion -- WouldReadPastEnd at :1666, :1688, :1720, :1755; ReadBytes bounds at
:1736 and :1741; values off the wire range-checked twice (:1669 and the serialize_int macro
at :1965). serialize_string_internal (:2347) is safe on read because length comes off the
wire via serialize_int bounded by buffer_size - 1; its assert at :2353 is inside an
IsWriting branch and never runs on read.
<!-- HOT:END -->

# CLAUDE.md

## What this is

A single-header C++ bitpacking serializer (~3,200 lines of library code in
[serialize.h](serialize.h), plus ~2,400 lines of embedded tests) aimed at game
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

## The 2026-08 performance program — what was measured and what binds

The fixed-point/128-bit + optimization wave (PRs #25–#30) left paid-for facts that future
performance work here must not relearn:

- **The inline threshold is the terrain.** The raw bitpacker fully inlines and SROAs its
  state into registers — micro-level restrict/aliasing work buys nothing there (measured,
  twice: 2026-07-21 and again in codegen 2026-08-07). But packet-sized `Serialize<Stream>`
  bodies OUTLINE at -O3, and at those boundaries `uint8_t*` (char aliasing) forces member
  state through memory — that is where restrict-qualified `this` paid +37.8% stream write
  and up to +152% in schema-generated writes. A stored measured-negative is dated evidence
  about a SHAPE: re-measure when inlining boundaries move.
- **The compile-time (const-params) forms** (#25) are wire-identical to the runtime forms
  (proven both directions) but calling them DIRECTLY from generated code was measured
  SLOWER (−33%) than generation-time constant folding — shared template instantiations
  outline; repeated bounds are the norm. They remain the human-facing surface; a
  force-inline pass on them is the flagged follow-up if direct calls are ever wanted.
- **The C++03 consumer floor is real and CI-enforced** (the cxx03 legs): a vendored
  serialize.h must compile in consumers' loosest modes — the emulated-128 conversions and
  any `constexpr` use need guards. yojimbo's debug make leg builds pre-C++11; its vendor
  trial caught the first violation (the trial is the consumer-side compat instrument —
  keep using it before releases).
- **WriteBytes packs head/tail bytes as whole values** (#27); its isolated chat-write win
  did NOT survive composition with generated code (unattributed residual on record) —
  isolated wins here must re-prove themselves in schema's four-language bench before
  being claimed.
- **The codec never divides** — the emulated-128 `operator/`/`%` exist for tests only;
  div-by-zero is documented UB with the differential exclusion commented permanent.

## Honest assessment

### Verified state (July 2026)

- All tests pass in Debug and Release on Linux x64, macOS Apple Silicon,
  Windows x64, and big-endian s390x (GCC cross-compile under QEMU), on
  every push, and the conformance corpus runs with them.
- The golden wire-format test proves all four platforms — including big
  endian — produce and decode byte-identical wire data.
- All tests pass under ASan + UBSan including the alignment sanitizer.
- Fuzzing (hostile read + differential write→read round trip): 60 seconds
  per push, 1 hour nightly with a cumulative corpus. No findings to date.
- Compiles clean with `-Wall -Wextra -Wpedantic`. `-Wconversion -Wshadow`
  produces ~80 warnings — implicit narrowing is a deliberate style here
  (the header disables MSVC C4244 for the same reason).
- The version lives in two files and a CI job compares them: `project(serialize VERSION ...)`
  in CMakeLists.txt and `SERIALIZE_VERSION` plus its MAJOR/MINOR/PATCH triple in serialize.h.
  Bump both together.
- The shared conformance corpus is `conformance/`, one file per covered operation, and
  [conformance.cpp](conformance.cpp) runs every vector in it through this library's reader as the
  ctest target `conformance`. The runner discovers the directory rather than naming files, which
  STANDARD.md requires of every runner in the family: here the glob is at cmake configure time,
  with CONFIGURE_DEPENDS so an added or removed file re-runs it, and an empty directory is a
  configure error. It is deliberately not generated from this code: a suite that regenerates its
  own expectations proves only that the library agrees with itself.
- The write side debug assertions are tested by [asserts.cpp](asserts.cpp), the ctest target
  `asserts`. It defines `serialize_assert` itself, so an assertion firing is an observable result
  rather than a process death and the target runs in Release too. A macro that narrows the
  caller's value must assert on the caller's original expression first: an assertion after a
  narrowing sees a value the narrowing already made legal.

### What's genuinely good

- **The read path is defensive, and recently hardened.** Every `ReadStream`
  operation bounds-checks before reading and range-checks after
  ([serialize.h:1655](serialize.h:1655)), returning false instead of asserting,
  so malicious packets fail cleanly. Arithmetic that could overflow signed
  ints is done in the unsigned domain with comments explaining why (e.g.
  [serialize.h:1486](serialize.h:1486), [serialize.h:1506](serialize.h:1506)),
  and NaN is clamped before any float-to-int cast
  ([serialize.h:2149](serialize.h:2149)). Recent commits show active work here.
- **The tests cover adversarial cases, not just round trips**: out-of-range
  encodings smuggled into bit headroom, full `[INT32_MIN, INT32_MAX]` ranges,
  negative and huge byte counts, NaN input, >2^31 relative gaps
  ([serialize.h:3747](serialize.h:3747) onward). This is better test thinking
  than most serialization libraries have.
- **The core design is sound and well understood.** Writer: 64-bit scratch,
  64-bit flush — the scratch stores as a qword when it fills and the bits
  that spilled past 64 carry into the next scratch, so the flush branch runs
  half as often as a dword design; each word is stored via `memcpy` so the
  buffer needs no particular alignment.
  Reader: branchless —
  each read loads a 64-bit window at the current byte position and shifts by
  the bit remainder, carrying no state between reads except the bit index.
  This replaced a word-at-a-time reader, at the cost of the 8-bytes-past
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
- **The buffer contracts** (owner-approved, July 2026): write buffer
  sizes must be a multiple of 8 bytes — the writer flushes qwords, and
  bytes past the written data are only ever written as zeros. Read buffer
  allocations must extend at least 8 bytes past the end of the packet
  data — the reader loads 64-bit windows at byte granularity, and bytes
  past the end are loaded but never interpreted. This is what makes the
  qword-flush writer and the branchless reader possible. Documented on
  the constructors. Do not propose removing these contracts or adding
  tail handling to avoid them.
- `serialize_int_relative` requires strictly increasing values over the domain
  0 to 2^31 - 1, and has no wrap semantics.
- **A refused read leaves a scalar destination unwritten, and failure is
  terminal**: the stream refuses every later read until it is re-initialized.
  A read into a caller-owned buffer (`bytes`, `string`, `wstring`) leaves that
  buffer unspecified, and the copy paths are not restructured for it.
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

