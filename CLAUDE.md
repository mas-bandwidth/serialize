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
- `serialize_int_relative` requires strictly increasing values.
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
  every push.
- The golden wire-format test proves all four platforms — including big
  endian — produce and decode byte-identical wire data.
- All tests pass under ASan + UBSan including the alignment sanitizer.
- Fuzzing (hostile read + differential write→read round trip): 60 seconds
  per push, 1 hour nightly with a cumulative corpus. No findings to date.
- Compiles clean with `-Wall -Wextra -Wpedantic`. `-Wconversion -Wshadow`
  produces ~80 warnings — implicit narrowing is a deliberate style here
  (the header disables MSVC C4244 for the same reason).
- Header and CMake version is 1.4.3 (`SERIALIZE_VERSION`), matching the
  v1.4.3 tag and GitHub release (latest, July 2026; v1.4.1 was skipped).
  1.4.3 tightens the write buffer contract to multiple-of-8 sizes (docs
  and debug assert; no behavior change for conforming buffers). 1.4.2
  carries the qword-flush writer (~25% faster writes), the
  symmetric write-side allocation contract, and the README limitations
  fixes. 1.4.0 carries the branchless
  reader and its breaking allocation contract change (read buffers must
  extend 8 bytes past the data, previously round-up-to-4), plus 64-bit bit
  counts throughout, which removes the old 256 MB buffer limit
  (test_large_buffer round trips across the old 2^31-bit boundary); the
  wire format is unchanged.
- Throughput ([bench.cpp](bench.cpp), Release, Apple Silicon reference):
  bitpacker write ~5.8 GB/s, read ~8.1 GB/s; stream write ~47M packets/s,
  read ~140M packets/s. (Reads got ~4x faster in 1.4.0 with the branchless
  reader; writes ~25% faster in 1.4.2 with the 64-bit flush.)

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
  half as often as a dword design (~+25% write throughput, measured); each
  word is stored via `memcpy` so the buffer needs no particular alignment.
  Reader: branchless —
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
- **The buffer contracts** (owner-approved, July 2026): write buffer
  sizes must be a multiple of 8 bytes — the writer flushes qwords, and
  bytes past the written data are only ever written as zeros. Read buffer
  allocations must extend at least 8 bytes past the end of the packet
  data — the reader loads 64-bit windows at byte granularity, and bytes
  past the end are loaded but never interpreted. This is what makes the
  qword-flush writer and the branchless reader possible. Documented on
  the constructors. Do not propose removing these contracts or adding
  tail handling to avoid them.
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

## Future work: rANS entropy coding (researched 2026-08-13, NOT implemented)

Glenn asked to look into rANS for serialize and record it for whenever we implement.
**Nothing here is built. This is a decision record so the next pass starts from evidence.**

### What it is, and why it is interesting here

**ANS (Asymmetric Numeral Systems), Jarosław Duda, 2013–2014.** `rANS` is the range variant.
Glenn's framing is correct: **it is mathematically equivalent to a range coder** — same
compression ratio as arithmetic/range coding to within a rounding error — **but much faster on
modern hardware.**

The speed comes from two places, and the second is the one that matters:

1. Decode is a single multiply plus a table lookup, with no division in the fast path.
2. **It interleaves.** A range coder has a serial renormalisation dependency: symbol N+1's
   decode waits on symbol N. rANS lets you run several independent coder states over one
   output buffer, which removes the serial chain and lets the decoder saturate the pipeline —
   and that is what makes SIMD worth anything here. Giesen measured an SSE4.1 decoder on an
   8-way interleaved stream at **~6.0 clocks/symbol (~540 MB/s)**, with a 16-way AVX2 variant
   faster still.

**THE CONSTRAINT TO DESIGN AROUND: rANS is LIFO.** The encoder emits in the reverse of decode
order. For a streaming bitpacked serializer whose whole shape is a single forward pass, that is
a real structural cost, not a detail — it means buffering a message (or a bounded block) before
the entropy stage. Decide that before writing any coder.

**And it needs a probability model.** serialize today is bitpacking: it spends exactly the bits
a range implies. rANS only pays off where the *values* are skewed within their range, so the
win depends entirely on a model of the data. Static per-field tables compiled from a schema are
the natural fit here and the cheapest thing to try first — which is why this note also lives in
[schema](https://github.com/mas-bandwidth/schema).

### Sources worth reading first

- **Fabian Giesen's `ryg_rans`** — https://github.com/rygorous/ryg_rans (public domain / CC0),
  plus *"rANS in practice"* (2015). **Read his 2026-08-03 post *"ryg_rans is not a library"*
  before copying anything**: he states plainly that it is reference code demonstrating the
  idea, and that the SIMD versions in particular are illustrations, not production code.
- **Charles Bloom** — Oodle LZNA and his entropy-coding posts; adaptive and semi-static models
  combined with interleaved rANS. He and Giesen developed much of this in parallel.

### PATENTS — the part that decides whether we may use it at all

Glenn's instruction: *"if it is patented then we must not."* The honest answer is **"mostly
clear, with one real hazard,"** and it needs a lawyer's read before we ship, not mine.

- **Duda never wanted it patented** and worked actively to keep ANS in the public domain.
- **Google's ANS application was rejected by the USPTO in 2018** after Duda filed third-party
  prior art, and Google abandoned the claim in the US and Europe.
- **Microsoft was granted `US11234023B2` in January 2022** — *"Features of range asymmetric
  number system encoding and decoding."* It does **not** claim rANS itself (Duda's 2013–2014
  publications are prior art). It claims **specific refinements**: a two-phase organisation of
  RANS decoding aimed at hardware, and adaptations for particular symbol distributions.
- **Microsoft has publicly stated** that anyone using it in an **open source codec that does
  not charge a license fee** has their permission.

**THE HAZARD IS THAT LAST CONDITION, AND IT POINTS STRAIGHT AT US.** serialize is BSD-3 today,
so the stated permission would cover it as it stands. But the declared direction for these
libraries is a move to **MBSL** (see netcode.cs: *"AGPL-3.0 for now; intended to move to MBSL
when ready"*), and a source license that charges a fee is exactly the case Microsoft's
permission is worded to exclude. **A grant we would lose precisely when the business succeeds
is not a grant to build a wire format on.** It is also a public statement rather than a filed
covenant or a patent pledge, which is a weaker instrument than it sounds.

### If we do it, the shape that avoids the hazard

1. Implement **baseline rANS as Duda published it** — the prior art, unencumbered.
2. **Do not implement the claimed refinements** in `US11234023B2` (the two-phase decode
   organisation, the distribution-adaptation features). Read the claims, not the abstract.
3. **Get a patent lawyer's opinion before release**, given the MBSL direction. This is the step
   that actually answers Glenn's question, and nothing above substitutes for it.
4. Keep it **optional and versioned in the wire format** — an entropy stage that cannot be
   turned off is a licensing problem welded to the protocol.
