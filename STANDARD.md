# serialize

This document specifies the **wire format** produced and consumed by the
serialize library, precisely enough to write an independent implementation that
interoperates byte-for-byte.

It describes a format, not an implementation. Nothing here constrains how you
structure your code.

## Format version

**The format version is 1.1.** It names the wire, not a library release. Two
endpoints interoperate when they run releases carrying the same format version,
and a release states which format version it implements.

The rulings that moved the format off 1.0, which was this document as first
written:

* **2026-08-15.** A degenerate range costs zero bits on every storage width.
  `wstring` transmits UTF-16 code units, with surrogate conversion at the
  boundary on a 4-byte `wchar_t` platform. Readers refuse malformed `string`
  and `wstring` payloads.
* **2026-08-23.** `compressed_float` clamps the quantized integer to
  `max_integer_value`.
* **2026-09-04.** `int_relative` carries the non-negative int32 domain, and
  every tier's reconstruction is refused outside it.

Every ruling in this list is part of format version 1.1, the 2026-09-04
refusal ruling included, and none of them moves the version again.

## Architecture

serialize is a **bit packer**. Values are written as variable numbers of bits
rather than whole bytes, so a boolean costs one bit and an integer known to lie
in `[0,7]` costs three.

Reading and writing are expressed once, as a single templated function per
message type, instantiated against a write stream, a read stream, or a measure
stream. The measure stream computes a conservative bound on the size a message
would occupy, without producing bytes — its obligations are specified in "The
Measure Stream" below. Everything else in this document concerns the bytes on
the wire.

## General Conventions

All multi-byte quantities are **little-endian**.

The stream is accumulated in a **64-bit scratch word**. Bits are packed
**least-significant-bit first**: the first value written occupies the lowest
bits of the first word. When the scratch fills, the 64-bit word is copied to
the buffer in host byte order on little-endian machines, and byte-swapped on
big-endian machines. The result is the same bytes on the wire everywhere.

A value of `n` bits, written when the scratch already holds `s` bits, occupies
bits `[s, s+n)` of the current word. A value that would cross the 64-bit
boundary is split: the low `64-s` bits complete the current word, and the
remainder begins the next.

**Flush.** After the final value, any partially filled scratch word is written
out. The stream therefore always occupies a whole number of 8-byte words in the
writer's buffer, but the meaningful length is the number of bytes actually
required, rounded up to a byte.

**Bit index and alignment.** The bit index is the count of bits written so far.
The stream is *aligned* when the bit index is a multiple of 8. The number of
bits needed to reach alignment is `(8 - (bit_index % 8)) % 8`.

**Writes assume trusted data — doctrine, ratified** *(adopted 2026-08-15 from
the schema enactment; the ruling verbatim: "this is an intentional design
choice of serialize. the write path is trusted, and it's your responsibility
as the user of serialize library to write correctly. asserts in languages that
support them in debug only.")*. Writer inputs are stated as **obligations, not
defined behaviors**: this document owes a conforming writer exact bytes and
owes a misbehaving writer nothing. Within that doctrine, misuse surfaces by
each implementation's own convention, and costlier contracts — the UTF-8
well-formedness contract under `string` is the type case, an O(n) check no
release path should carry — assert in checked builds, everywhere. A **checked
build** is a build with assertions enabled. This document uses that one term
wherever a check depends on the build. The read side is untouched by the
doctrine: readers face untrusted data, and every refusal rule this document
states binds in every build mode.

## Bit-Level Primitives

### bits

    serialize_bits( stream, value, bits )

Writes the low `bits` bits of `value`, where `bits` is in `[1,64]`.

* For `bits <= 32` this is a single group of that many bits, and the value must
  be less than `2^bits`.
* For `bits > 32` the value is split: the **low 32 bits are written first as a
  32-bit group**, then the remaining `bits - 32` high bits as a second group.

The bound `value < 2^bits` holds at every width in `[1,64]` and not only at 32
or fewer. A `bits` call of width zero is caller error rather than a zero-bit
field, and the ranged operations below branch around this primitive when the
range is degenerate.

Fixed-width helpers are aliases for exactly this, and carry no range
information of their own:

| helper | equivalent to |
|---|---|
| `serialize_uint8( value )`  | `serialize_bits( value, 8 )` |
| `serialize_uint16( value )` | `serialize_bits( value, 16 )` |
| `serialize_uint32( value )` | `serialize_bits( value, 32 )` |
| `serialize_uint64( value )` | `serialize_bits( value, 64 )` — low 32 then high 32 |

### bool

    serialize_bool( stream, value )

One bit: `1` for true, `0` for false.

### uint128

    serialize_uint128( stream, value )

A 128-bit unsigned integer, always 128 bits on the wire: the **low 64-bit half
first**, then the high half, each half written exactly as
`serialize_bits( half, 64 )` — that is, four 32-bit groups from least
significant upward. When the stream is byte aligned, the result is the 16
bytes of the value in little-endian order.

The operation is representation-independent. A native `unsigned __int128`, the
library's emulated two-lane type (`lo` then `hi`, both `uint64_t`), and two
explicit `serialize_uint64` calls (low half first) all produce **byte-identical
wire**. An implementation with no 128-bit type reproduces the format exactly
with two 64-bit operations.

### align

    serialize_align( stream )

Pads with **zero bits** until the bit index is a multiple of 8. If the stream
is already aligned, **nothing is written**.

Readers must verify that the padding bits are zero and fail the read if they
are not. This makes malformed streams detectable rather than silently accepted.

## Integers

### int (ranged)

    serialize_int( stream, value, min, max )

The defining operation of the format. The number of bits used is determined
entirely by the range:

    bits_required( min, max ) = ( min == max ) ? 0 : 32 - count_leading_zeros( max - min )

The arithmetic is unsigned and wrapping: `max - min` and `value - min` are
computed in the unsigned domain of the operation's width, `uint32` for `int`,
`uint64` for `int64` and `uint128` for `int128`, and the leading zero count is
taken at that same width, so a range as wide as `[INT32_MIN, INT32_MAX]` is
exact rather than overflowing.

The wider widths carry the same rule under their own names.
`bits_required64( min, max )` is zero when `min == max` and otherwise
`64 - count_leading_zeros64( max - min )` over the unsigned 64-bit difference,
and `bits_required128` is that rule again at 128 bits.

`value - min` is written in that many bits. Note the consequences:

* a range of `[0,7]` costs 3 bits;
* a range of `[0,8]` costs 4 bits;
* a degenerate range where `min == max` costs **zero bits** — the value is
  known from the range alone and nothing is written.

`min <= max` is the legal relation, in every build mode and for every ranged
operation in this document: `int`, `int64`, `int128` and `fixed`. The
degenerate range is a field a conforming implementation must accept, not a
misuse, so a checked build must assert `min <= max` and never `min < max`. The
writer emits nothing, the reader consumes nothing and takes the value from
`min`, and a measure adds zero bits.

`compressed_float` is not a ranged operation and is excluded. It takes bounds
but quantizes across them, and a zero `delta` has no quantization to define, so
it requires `min < max` and a checked build asserts that.

Readers must check that the decoded value lies within `[min,max]` and fail
otherwise. The check is stated in offset form for `int`, `int64` and `fixed`
alike: the reader compares the decoded offset against `max - min` in the
unsigned domain before it forms the value, which cannot overflow on hostile
input and is equivalent to the value form.

The range must be identical on both sides. The format carries no
self-description: a stream is only interpretable by a reader that performs the
same sequence of operations with the same parameters.

### int64 (ranged)

    serialize_int64( stream, value, min, max )

The 64-bit counterpart of the ranged integer, and the only ranged 64-bit
operation. `bits_required64( min, max )` bits are used. If that is 32 or fewer,
the value is written as a single group of that many bits; otherwise the low 32
bits are written first, followed by the remaining `bits - 32` high bits.

**Do not confuse this with `serialize_uint64`**, which is not ranged — it is
`serialize_bits( value, 64 )` and always costs a full 64 bits. The names are
similar and the encodings are not.

Readers must check that the decoded value lies within `[min,max]` and fail
otherwise, in the offset form stated under `serialize_int`.

### int128 (ranged)

    serialize_int128( stream, value, min, max )

The 128-bit counterpart, and the only ranged 128-bit operation.
`bits_required128( min, max )` bits are used, where `min` and `max` are
converted to the unsigned 128-bit domain first, so a range wider than `2^127`
is exact rather than overflowing. The offset `value - min` is computed in that
same unsigned domain and written in 32-bit groups from least significant
upward — the same splitting rule as `serialize_bits` and the wide fixed point
path: `bits <= 32` is a single group, otherwise full 32-bit groups from the
bottom with the final group carrying the remainder, up to four groups.

Where the range fits 64 bits or fewer the bytes are **identical to
`serialize_int64( value, min, max )`** over the same bounds. A field may
therefore be widened from 64 to 128 bits without changing the wire, provided
the bounds do not change.

The bounds are runtime values, exactly as for `serialize_int` and
`serialize_int64`. The bit count comes from the runtime `bits_required128`,
which is available on every platform — including compilers with no native
`__int128`, where the emulated pair supplies every operation it needs.

**Do not confuse this with `serialize_uint128`**, which is not ranged — it is
two `serialize_uint64` calls and always costs a full 128 bits.

Readers must check that the decoded offset is at most `max - min` in the
unsigned domain and fail otherwise — reject, never clamp.

**A degenerate range where `min == max` is legal and costs zero bits, on the
128-bit width exactly as on the narrower ones.** `bits_required128( min, max )`
is zero, and the rule stated under `serialize_int` applies unchanged: `min <=
max` in every build mode, nothing on the wire, and the value taken from `min`.

### fixed (Q format, ranged)

    serialize_fixed( stream, value, integer_bits, fraction_bits, min, max )

A fixed point value held in an integer storage type of exactly `integer_bits +
fraction_bits` bits, with the sign bit counting toward `integer_bits` (Q48.16
in an `int64_t`, Q112.16 in an `__int128`). The stored integer is the real
value scaled by `2^fraction_bits`. `min` and `max` are bounds in **whole real
units**, and all four parameters are compile-time constants of the call site —
they are part of the format, exactly like a ranged integer's bounds.

The legal storage widths are 8, 16, 32, 64 and 128 bits, always signed, and
`integer_bits` is at least 1 because the sign bit counts toward it.
"Compile-time constants" describes the C++ call site rather than the wire, so a
language whose generics cannot take integer parameters passes the four
parameters at runtime and produces the same bytes. `value` is a raw backed
fixed point value of the storage type rather than a floating point number, so
the operation performs no conversion of its own.

The encoding is an offset encoding over the **raw** (scaled) bounds:

    raw_min = min << fraction_bits
    raw_max = max << fraction_bits
    bits    = bit length of ( raw_max - raw_min )    — bits_required, at whatever width the range needs

The raw range lives in the unsigned domain of the storage width, exactly as
`int128`'s bounds do, so a Q64.64 field over the full `int64` unit range has an
exact raw range of `2^128 - 2^64` rather than an overflowing one.

`raw_value - raw_min` is written in `bits` bits, split into 32-bit groups from
least significant upward exactly as `serialize_bits` splits wide values:
`bits <= 32` is a single group, otherwise full 32-bit groups from the bottom
with the final group carrying the remainder. For storage of 64 bits or fewer
the bytes are **identical to `serialize_int64( raw_value, raw_min, raw_max )`**
— fixed point adds no new wire structure, only the compile-time scaling
convention — and with `fraction_bits = 0` the operation *is* a ranged integer.

Readers must check that the decoded offset is at most `raw_max - raw_min` and
fail otherwise — reject, never clamp.

**A degenerate range where `min == max` is legal and costs zero bits — on
every storage width** *(adopted 2026-08-15 from the schema enactment)*. The
wire carries nothing and the reader recovers the value from the range alone:
the raw value is `min << fraction_bits`, exactly the rule the ranged integers
have always stated. The storage width must not change this: a Q112.16 field
over a degenerate range costs zero bits, not `fraction_bits` zeros. *(Until
2026-08-15 the implementations behaved four different ways here — zero bits,
`fraction_bits` of zeros on the wide path only, a compile failure and a
panic — the divergence the ruling closes.)*

Because fixed point values are integers underneath, the round trip is
**exact**: unlike `compressed_float` there is no quantization step, and the
same raw value produces the same bytes and reads back bit-for-bit identical on
every platform.

**The one rounding rule** *(adopted 2026-08-15 from the schema enactment)*:
the wire itself never rounds — the round trip above is exact — but wherever a
value is quantized into a Q format or narrowed out of one, fixed point rounds
ties **half away from zero**, everywhere it rounds: `( raw + half ) >> drop`
for `raw >= 0`, `-( ( -raw + half ) >> drop )` for `raw < 0`, with
`half = 1 << ( drop - 1 )`. The hazard, named: the naive arithmetic shift
*floors*, so an implementation that applies `( raw + half ) >> drop` to a
negative raw rounds ties toward +infinity and diverges by exactly one raw
step, on exact ties of negative raws only. A rounding rule is not wire shape —
no protocol identifier can see the divergence — so a conformance vector must
pin a negative tie value that distinguishes the two rules. The rule binds every
conversion between floating point and fixed point that a runtime chooses to
expose, and never a byte on the wire.

### int_relative

    serialize_int_relative( stream, previous, current )

Encodes an increasing sequence compactly, where `current > previous`. Let
`difference = current - previous`. The encoding is a ladder of one-bit flags,
each answering "does it fit in this tier?":

| tier | flag sequence | payload | difference range |
|---|---|---|---|
| `one-bit` | `1` | — | exactly 1 |
| `bounded-3` | `0 1` | `serialize_int( d, 2, 6 )` — 3 bits | 2 – 6 |
| `bounded-5` | `0 0 1` | `serialize_int( d, 7, 23 )` — 5 bits | 7 – 23 |
| `bounded-9` | `0 0 0 1` | `serialize_int( d, 24, 280 )` — 9 bits | 24 – 280 |
| `bounded-13` | `0 0 0 0 1` | `serialize_int( d, 281, 4377 )` — 13 bits | 281 – 4377 |
| `bounded-17` | `0 0 0 0 0 1` | `serialize_int( d, 4378, 69914 )` — 17 bits | 4378 – 69914 |
| `absolute` | `0 0 0 0 0 0` | `current` as 32 raw bits | anything |

The tier names are this document's, and the conformance vectors use them. The
five `bounded-*` tiers are named for their payload width.

**The writer takes the first tier the difference fits, and that encoding is the
canonical one.** The reader accepts any tier whose payload decodes within the
rules, so a stream carrying a difference in a wider tier than a writer would
have chosen is a valid stream rather than a refused one.

A difference of 1 — the common case for sequence numbers — costs a single bit.

**The final tier transmits `current`, not the difference.** Every tier above it
encodes the difference, so this reads as an inconsistency and is not: at full
width the subtraction buys nothing, and sending the absolute value lets the
reader check `current > previous` directly. The absolute form carries no
ordering guarantee of its own, so the reader checks it under the reconstruction
rule below.

**The semantics are pinned: no wrapping** *(adopted 2026-08-15 from the schema
enactment; the ruling verbatim: "no wrapping sequence numbers. meant for
positive only and up to maximum only.")*. `serialize_int_relative` is strictly
increasing — `current > previous`, the reader fails otherwise — and no wrap
semantics exist: a caller with a wrapping counter unwraps it before
serializing. Wrap-around is not an encoding this operation carries, not now
and not by future amendment.

**The domain is the non-negative int32 range, `0` to `2^31 - 1` inclusive.**
Both `previous` and `current` lie in it. The domain is a property of the
operation, not of the caller's storage type: a 64-bit or unsigned `previous` of
`2^31` is caller error everywhere, exactly as a negative one is. `previous` is
the caller's own state and never arrives off the wire, so a `previous` outside
the domain is caller error, asserted in checked builds, and this document
defines no wire meaning for it. The operation's API type is the signed 32-bit
integer, and the domain binds whatever wider or unsigned storage a caller keeps
its sequence in.

**Every tier's reconstruction must be checked.** The reader must reconstruct
`current` in a width that cannot wrap, then compare the result against the
domain and against `previous`, and must refuse the read unless `current` lies
in the domain and is strictly greater than `previous`. That binds in the
`one-bit` tier, in each of the five `bounded-*` tiers, and in the `absolute`
tier.

**The `absolute` tier's 32 raw bits are unsigned.** The group must be read as
an unsigned 32-bit value, so a value with the top bit set is outside the domain
and the rule above refuses it. A reader that reads the group into a signed
sequence type first has already left the domain, and the two readings disagree
about the same byte sequence.

The refusal outcome is the one Reader Obligations states for every operation.

## Floating Point

### float

    serialize_float( stream, value )

The 32 bits of the IEEE-754 single-precision representation, written as a
32-bit group. No conversion, no compression.

### double

    serialize_double( stream, value )

The 64 bits of the IEEE-754 double-precision representation, written as one
64-bit group.

**Bit transparency — both directions** *(ratified 2026-08-15 from the #56
re-audit; every implementation already complies)*. `float` and `double` are
transparent in both directions. Every bit pattern is legal on the wire — NaNs
with any payload, signaling NaNs, infinities, negative zero, denormals — and
the reader reproduces the transmitted pattern exactly: it must not
canonicalize, quiet, flush, or refuse any of them. There is no reader
latitude here — a reader that rejects NaN, or canonicalizes payloads, does
not conform; the read returns exactly the bits read. A round trip through any
conforming writer/reader pair preserves all 32 (or 64) bits. Conformance
vectors for these operations must compare **bit patterns, not values**: NaN
compares unequal to itself, `-0.0 == 0.0`, and a tolerance comparison cannot
see a quieted signaling bit, so a value-space comparison here proves nothing.

### compressed_float

    serialize_compressed_float( stream, value, min, max, res )

A float quantized to a resolution. Let `delta = max - min` and
`values = delta / res`, clamped to `[1, 4294967040]` (the largest float below
`2^32`). Then:

    max_integer_value = ceil( values )
    bits              = bits_required( 0, max_integer_value )

The writer clamps `(value - min) / delta` to `[0,1]`, multiplies by
`max_integer_value`, adds `0.5`, takes the floor, **clamps the resulting
integer to `max_integer_value`**, and writes the result in `bits` bits. The reader divides by `max_integer_value`, multiplies by `delta`,
and adds `min`.

`delta`, the quotient `delta / res` and the clamp are computed in `float32`
like the rest of this operation. A `res` of zero or below is caller error,
asserted in checked builds, and this document defines no wire meaning for it.

**This arithmetic is `float32`, and the two roundings are part of the format.**
The product `normalized * max_integer_value` rounds to `float32` BEFORE `0.5`
is added, and that sum rounds to `float32` before the floor. Two roundings, not
one. Specifically, an implementation must not:

- widen any step to `double` (or any wider type) before the floor, and
- contract the multiply and the add into a fused multiply-add, which rounds
  once instead of twice. Languages that permit contraction must suppress it
  here. In C and C++ a plain `float` local is **not** sufficient: it suppresses
  only statement-local contraction (clang's default `-ffp-contract=on`), and
  under `-ffp-contract=fast` — GCC's default at every optimization level — the
  compiler fuses straight through it. The rounding must be pinned by an
  optimization barrier on the stored product (the C++ implementation's
  `SERIALIZE_FLOAT_FORCE_ROUND`: an empty asm with a register output operand on
  GCC/clang, a `volatile` store where inline asm is unavailable) or by building
  with `-ffp-contract=off`. In Go an explicit `float32()` conversion around the
  product suffices — the spec forbids fusing across it. Rust does not fuse
  unless `mul_add` is called explicitly.

The rule is a property of the arithmetic rather than a list of languages: no
fused multiply-add across the expressions named here, in any language, and
every runtime documents the barrier it uses to hold that. The witness value
`8388608.0` and the two witness ranges named below are corpus vectors, carried
in `conformance/` like every other pinned vector.

**The integer clamp is normative — added 2026-08-23 (schema#109; ruling:
Glenn, live).** Once `max_integer_value >= 2^23` the `float32` ulp at the top
of the range reaches 1, so the rounded sum can exceed `max_integer_value`
itself. Without the clamp, 2,109,734,656 step counts emit a top-of-range code
the reader's own `integerValue > max_integer_value` check rejects, and 128
step counts emit a code one bit wider than the field — where implementations
historically diverged on the wire (the C++ implementation leaked the extra
bit into the stream; serialize.cs masked it to zero). The clamp closes both
classes, costs one comparison on a path already doing a floor, and changes no
byte for any declaration outside `[2^23, 2^24)`. Witnesses every
implementation must pin, writing `max`: `[0, 8388609]` at resolution `1` (the
reader-rejects class) and `[0, 16777215]` at resolution `1` (the
wire-divergence class).

This is not pedantry; it changes the bytes. Over `[0, 10]` at resolution
`0.01`, the required arithmetic quantizes `0.005` to `1`, `0.025` to `3`,
`0.105` to `11` and `9.995` to `1000`; widening to `double` yields `0`, `2`,
`10` and `999`. A value landing exactly on a quantum — `2.5` here — agrees
under every variant, so a conformance vector built only from such values will
pass while the wire is wrong. Vectors must include values that land between
quanta.

The between-quanta values above discriminate a **widened** writer; a **fused**
writer is a separate class with its own discriminating band. Where
`max_integer_value` is below `2^23` the product's ulp is well under the `0.5`
being added and fusion almost never moves the integer; once
`max_integer_value` reaches `[2^23, 2^24)` — the integer clamp's band — the
product's ulp reaches `1` and fusion moves the quantized integer on mass:
measured exhaustively over `[0, 16777215]` at resolution `1`, a fused writer
moves 4,194,304 inputs (every even `float` in the top binade), the first being
`2^23` itself. Conformance vectors must include a value from this band —
`8388608.0` over that declaration is the pinned witness — or a fused writer
passes every vector below the band while shipping divergent wire above it.

**The reader's arithmetic is pinned the same way.** The decode — divide by
`max_integer_value`, multiply by `delta`, add `min` — is `float32` with every
step rounding: the quotient rounds, the product rounds BEFORE `min` is added,
and the sum rounds. An implementation must not widen any step to `double`, and
must not contract the multiply and the add into a fused multiply-add — fused,
the decode rounds once instead of twice, and whenever `min` is non-zero the
decoded value can land one ulp away from the conformant result. That never
changes the bytes being read, but it changes the value obtained from them: a
value decoded on a fusing platform and re-encoded produces different wire,
which breaks round-tripping between conforming implementations. The same
suppression techniques the writer paragraph lists apply here, and conformance
vectors over a non-zero-`min` range must pin decoded values **bit-exactly** —
a tolerance comparison cannot see a one-ulp divergence.

Readers must reject an integer greater than `max_integer_value`.

**Non-finite inputs are non-conforming.** *(Adopted 2026-08-15; the ruling
verbatim: "it's non-conforming. also, attempting to send NaN or INF or
anything else through compressed float is non-conforming and should assert
out on write too.")* A declaration whose `delta = max - min` — or whose
`values = delta / res` — is not finite in `float32` is non-conforming, and
this document defines no wire meaning for it. Writing a non-finite value
(NaN, `+Inf`, `-Inf`) through `compressed_float` is non-conforming.
Conforming writers assert in checked builds, per the family's writer-trusted
model. The read path is untouched: the poison is in the declaration or the
input value, never on the wire, so there is nothing for a reader to refuse.

This is lossy by construction: a round trip returns the nearest representable
quantum, not the original value.

### object

    serialize_object( stream, object )

Invokes the object's own serialize function inline. It contributes **no bytes
of its own** — it is composition, not an encoding. Whatever the nested object
writes appears at exactly this position in the stream, with no framing, length
prefix, or alignment inserted around it.

## Bytes and Strings

### bytes

    serialize_bytes( stream, data, count )

**Aligns first**, then writes `count` raw bytes. The alignment is part of the
format, not an optimization — a reader that does not align will desynchronize.

`count` is not written. Both sides must already agree on it.

**`count` may be zero** *(ratified 2026-08-15 from the #56 re-audit; every
implementation already complies)*. The alignment is performed regardless: a
zero-length `serialize_bytes` pads to the byte boundary and writes nothing
else, and the reader performs and verifies the same alignment. Skipping the
alignment when `count` is zero — a plausible "optimization" — desynchronizes
every field that follows, and a round-trip self-test cannot catch it, because
both halves skip the same align. This clause is load-bearing for `string`,
whose empty case reaches exactly this path.

### string

    serialize_string( stream, string, buffer_size )

A null-terminated narrow string.

1. The length, as `serialize_int( length, 0, buffer_size - 1 )`. The bit cost
   therefore depends on `buffer_size`, which both sides must agree on.
2. The characters, as `serialize_bytes` — **which aligns**.

The terminator is not transmitted; the reader appends it.

Because `buffer_size` is an operand rather than a transmitted value, the same
string serialized against different buffer sizes produces different bytes.
`buffer_size` is a wire operand that bounds the length field and describes no
destination, the destination is the language's own string type, and `length`
counts bytes.

**`string` payloads are well-formed UTF-8 by contract** *(adopted 2026-08-15
from the schema enactment, writer-trusted per the doctrine above)*. The wire
shape is unchanged; what the `string` spelling adds is a **contract**: the
payload is well-formed UTF-8, the writer's obligation. Writing malformed UTF-8
is a writer contract violation, asserted in checked builds where the language
supports them, and the conformance vectors carry only valid UTF-8. An
application with genuinely arbitrary payloads uses `serialize_bytes`, which
remains exactly that. *(Until 2026-08-15 this paragraph also promised no
mandatory read-path validation; the ruling below supersedes that half. The
writer's obligation stands unchanged.)*

**Readers must refuse malformed `string` payloads** *(adopted 2026-08-15)*.
Two refusal rules, binding in every build mode like every other read-side rule
in this document:

* **Invalid UTF-8 fails the read.** The payload the contract above promises is
  the payload the reader insists on: a stream carrying malformed UTF-8 was not
  produced by a conforming writer, and the reader refuses it rather than
  handing it to the application. Invalid means not well formed under Unicode
  Table 3-7, so overlong forms, encoded surrogates, code points above
  `10FFFF` and truncated sequences are all refused.
* **An interior NUL fails the read** — a zero byte anywhere among the `length`
  transmitted bytes. A conforming writer derives `length` from `strlen`, so no
  zero byte can reach the wire from conformance; a stream carrying one gives
  the payload **two lengths** — the wire length, and the `strlen` every
  consumer downstream will compute — and everything between them rides
  invisibly past whichever side uses the other. Refusing the byte closes the
  smuggling primitive. (NUL is well-formed UTF-8, which is why this is its own
  rule.)

These are refusal rules, and refusal rules are format: an implementation that
skips one accepts streams a conforming implementation refuses. *(Cost,
recorded with the ruling: the string operations are the convenience path, not
the hot path — strings are rare in serialized game traffic, and a design
chasing minimal bandwidth does not send strings at all — so the O(n)
validation prices into traffic that performance-sensitive designs do not
carry.)*

### wstring

    serialize_wstring( stream, string, buffer_size )

A null-terminated wide string. `buffer_size` counts **wide characters, not
bytes**. `wstring` is a required operation in every implementation, its
destination is the language's own string type, and a runtime whose strings are
not UTF-16 recombines surrogate pairs into code points on read.

1. The length, as `serialize_int( length, 0, buffer_size - 1 )`.
2. Each character as a **32-bit group**, in order.

**No alignment is performed anywhere in this operation** — this is the one
place where the wide-string path deliberately differs from its narrow
counterpart, which aligns via `serialize_bytes`. An implementation that mirrors
the narrow string path here will produce the wrong bytes.

Wide characters are transmitted as 32 bits regardless of the local `wchar_t`
width. A group above `0xFFFF` is not a UTF-16 code unit, and **the reader must
refuse it on every platform**, whatever the local `wchar_t` can hold. Refusal
does not depend on the platform, so the same byte sequence is refused
everywhere.

**Each 32-bit group carries one UTF-16 code unit — not one code point — and
the payload is well-formed UTF-16 by contract** *(adopted 2026-08-15 from the
schema enactment, writer-trusted per the doctrine above)*. Surrogate **pairs**
are valid — full Unicode, an astral character is two groups; an **unpaired**
surrogate is a writer contract violation, asserted in checked builds where
the language supports it. 2-byte and 4-byte `wchar_t` platforms must produce
**identical bytes**: the 4-byte platform converts at the boundary — splits
astral code points into surrogate pairs on write, recombines on read — because
the platform-compatibility claim this section used to make was false for astral
text when each platform transmitted its own `wchar_t` units. Basic-plane text
is unaffected on every platform.

**Readers must refuse malformed `wstring` payloads** *(adopted 2026-08-15, the
same ruling as `string`'s)*. The same two rules, in UTF-16 terms, binding in
every build mode:

* **An unpaired surrogate fails the read**: a high surrogate
  (`0xD800`–`0xDBFF`) not immediately followed by a low surrogate
  (`0xDC00`–`0xDFFF`), a low surrogate not immediately preceded by a high
  surrogate, or a high surrogate as the final transmitted group. Well-formed
  surrogate **pairs** remain valid — they are how astral text travels.
* **An interior NUL fails the read** — a zero group among the `length`
  transmitted groups — by the same two-lengths logic as `string`: a conforming
  writer derives `length` from `wcslen`, so a zero group is impossible from
  conformance, and a stream carrying one is carrying a payload with two
  lengths.

## Worked Example

The library's golden test serializes a fixed message and asserts an exact
112-byte output. One field is a wide string in a `wchar_t[8]` buffer
containing three characters — `0x043C`, `0x0438`, `0x0440` — and it produces
this 13-byte run:

    0xE3 0x21 0x00 0x00 0xC0 0x21 0x00 0x00 0x00 0x22 0x00 0x00 0x00

Decoding it against this document:

* `buffer_size` is 8, so the length field is `serialize_int( length, 0, 7 )`,
  which is `bits_required(0,7)` = **3 bits**.
* `0xE3` is `1110 0011`. Its low 3 bits are `011` = **3**, the length.
* No alignment follows. The first character begins immediately at bit 3.
* The remaining 5 bits of `0xE3` are `11100` = `0x1C`, which is the low 5 bits
  of `0x043C`. The next byte `0x21` supplies `0x043C >> 5`. The character is
  **`0x043C`**.
* Two further 32-bit groups follow, yielding `0x0438` and `0x0440`.

Total: 3 + 3×32 = 99 bits = 13 bytes once the following align pads to the byte
boundary. This matches, and it is the cheapest way to confirm an independent
implementation is correct.

The message then aligns and continues with four fixed point fields. The first
is `serialize_fixed( value, 8, 8, -100, +100 )` — Q8.8, so `raw_min` is
`-100 << 8 = -25600`, the raw range is `51200`, and the field costs 16 bits.
The next two bytes of the golden vector are `0xC0 0x60`, which is the offset
`0x60C0 = 24768`; adding `raw_min` gives a raw value of `-832`, which is
`-3.25` in Q8.8 — exactly the value the golden message stores.

After another align the message ends with two wide fixed point fields that
make the multi-group split load-bearing: a Q112.16 field over ±2^57 whole
units (75 bits — two full 32-bit groups from the bottom, then the 11-bit
remainder on top), and a Q64.64 field over the full int64 unit range (128
bits — four 32-bit groups). A decoder that assembles the groups in the wrong
order, or puts the remainder anywhere but the most significant position,
decodes the wrong values here.

## Read-only and write-only forms

Every operation above has `read_` and `write_` variants — `read_string`,
`write_bits`, and so on — for code paths that only ever read or only ever
write, rather than sharing one templated function.

**They produce byte-identical output to their `serialize_` counterpart.** They
exist for convenience and to avoid a branch, not to encode anything
differently. This document therefore specifies each operation once, under its
`serialize_` name.

The API surface is not constrained. A single stream abstraction with a read, a
write and a measure conformer carries every operation in this document, and
these variants are one implementation's convenience rather than an obligation.

## The Measure Stream

Until 2026-08-15 this document disclaimed the measure stream in its third
paragraph and never mentioned it again — the same silence `compressed_float`
once enjoyed, and the implementations had quietly split under it: one measured
alignment exactly from a running bit index, four charged the worst case. This
section replaces the silence with the ruling *(2026-08-15, verbatim: "measure
must be large enough to serialize the message but doesn't need to be exact. it
is used only for yojimbo message serialization to see if there is enough room
in the packet to definitely serialize a message.")*.

**A measure is a bound, not the packet size.** A measure must report a size
**sufficient to serialize the message at any starting bit position**. It need
not be exact, and cannot be: alignment cost depends on the bit position the
message is later written at, which a measure does not know — the same message
costs different bits at different offsets, so no single number is exact for
all of them *(the ruling: "the exact is not possible, since align is going to
be different in 1st and 2nd times serialize is called. it is bit position
dependent.")*.

**The expected implementation charges the worst case: 7 bits per
alignment-performing operation** — `align`, `bytes`, `string` — and exact
width for everything else, which is every other operation: nothing else in
this document is position-dependent.

**A measure refuses nothing at runtime.** The measure follows the write
path's misuse model *(the writes-trusted doctrine above)*: invalid parameters
or out-of-range values are the caller's contract violation, asserted in
checked builds where the language supports it, and a measure never refuses at
runtime in release. A measure sits on the trusted side of the boundary —
nothing it sees came off a network.

**Exact-from-zero accounting is non-conforming** *(the ruling: "so if some
implementations of serialize measure in other languages are exact, they
probably should not be. make them conservative bounds like in C++, and the
standard should specify this is what is expected.")*. A measure that computes
alignment from a running bit index starting at zero reports the exact cost of
writing the message from an aligned start — and **under-counts every unaligned
start**. The worked example: `{ bits(8); align; bits(8) }` is 16 bits — 2
bytes — written from an aligned start, where the align is a no-op; written
from bit offset 1, the align pads 7 bits and the message spans 23 bits — 3
bytes of room needed. The exact-from-zero answer of 2 bytes is not sufficient
at every starting position, which is the one thing a measure is for. The
conservative answer — 8 + 7 + 8 = 23 bits — is sufficient everywhere.

**What a measure is for, and what it is not** *(rationale, recorded)*. The
operation exists so a packet assembler can ask "does this message definitely
fit in the space remaining?" — the yojimbo fits-check — and a conservative
bound answers exactly that question. Comparing a measure to a write's
`bytes_processed` and expecting equality is a misuse: the bound is not the
packet size. An application that needs the true bit count of a message from a
known starting position has always had the escape hatch — write it to a
scratch stream and read the count off the write. And the operation is probably
vestigial — *"we won't support it with the rANS encoder for example"* — 
specified here because all nine implementations ship it and had already begun
to disagree, not because it is expected to survive into entropy-coded
encodings.

**The measure stream is a required operation**, and every implementation ships
it. **Testable**: for every sequence vector in the shared corpus,
`measure >= bits written`, at every starting bit position; and the worked
example discriminates — a conservative measure reports 23 bits for
`{ bits(8); align; bits(8) }` where an exact-from-zero measure reports 16.

## Reader Obligations

The operations above state what the bytes mean. This section states what a
reader must **do**. These streams arrive from the network; for a parser of
untrusted input, whatever this document leaves unspecified is the attack
surface.

**Reading past the end must fail.** An operation that would consume more bits
than remain in the stream fails the read. It must not produce a partial value,
zero-fill the missing bits, or wrap. The failure is terminal under the rule
below. A stream's length is a count of bytes, so the test an operation performs
is `bit_index + n > 8 * bytes`, and `align` can never run past the end because
it never crosses a byte boundary.

**A trap, a crash or an abort on malformed input is non-conforming.** Refusal
is the only conforming answer to a stream this document says must be refused,
and a runtime whose integer arithmetic traps on overflow unconditionally uses
wrapping operations on the read path, so that no input can turn a refusal into
a trap.

**A refused primitive read must leave its destination unwritten.** The rule is
per primitive read: when a read of a scalar fails, the caller's value must be
exactly what it was before the call. A reader that assigns and then checks
leaves the caller holding a value the stream never carried, and a caller that
trusts the destination over the return code proceeds on it.

Two things the rule does not reach. A read into a caller-owned buffer, which is
`bytes`, `string` and `wstring`, leaves that buffer's contents **unspecified**
after a refusal, and no implementation restructures a copy path for it. A
composite read, which is `object` or any sequence of reads over an array, may
leave earlier members written, because it is a sequence of primitive reads and
each one carries the rule alone.

**Failure is terminal.** Nothing after a failing operation has a defined
position, so nothing after it is interpretable, and it must be the stream that
enforces that rather than the caller's discipline. Two shapes satisfy it:

* **By latch**, where a stream object survives a failure. The stream carries a
  failure state, the first failed read sets it, and every later read on that
  stream must fail, consuming no bits and writing no destination. Poisoning the
  position past the end of the buffer, so the existing past-end check refuses
  every later read, is an admitted implementation of the latch and the
  recommended one: it costs the read path nothing.
* **By construction**, where the failing read returns no successor stream or
  unwinds. An immutable stream whose failing read returns an error and no
  stream, and a reader that throws, satisfy the rule as written, because
  neither hands the caller a stream to continue on.

Both shapes conform, and the speed rule does not forbid a language's own idiom
for failure, so a runtime that reports failure by throwing conforms where
throwing is that idiom, with the poisoned position remaining the recommended
latch.

Every read consults the failure state before it does anything else, zero-bit
reads included, so a degenerate ranged read, an `align` on an already aligned
stream, a `bytes` call of zero count and `object` all refuse on a stream that
has already failed.

A failure persists until the stream is **re-initialized**, which is the
operation that points a stream at a new buffer, or until the stream is
discarded. An implementation with no re-initialization discards.

**Past-end memory is an implementation contract, not a format concern.** The
stream is exactly its stated length, and no operation's meaning ever depends
on memory beyond it. An implementation may still *load* — never interpret —
bytes past the end as an artifact of how it reads: the C++ and C
implementations both load 64-bit windows at byte granularity and therefore
require their caller to allocate at least 8 bytes beyond the data. That is
the accepted best practice, and Implementation Law's buffer contract holds
implementations to it: machinery that avoids the slack requirement at the
cost of per-operation work in the hot path is a slower correct option:
conforming on the wire, refused as an implementation choice by the speed rule.
The format turns on none of this. Conformance requires that
loaded-but-uninterpreted bytes can never influence a decoded value or an
accept/reject decision, and that an implementation state which
allocation contract its caller is under — a caller holding the wrong contract
is reading out of bounds, and that is a property of the implementation's
documentation, not of the wire.

The eight-byte slack contract is the fast path, and every runtime states that
its callers are under it. A copying entry point that takes an exact-length
input and copies it into a slack backed buffer is admitted as a convenience, as
the C implementation's padded wrapper is, because it prices the copy once at
the boundary rather than per operation in the hot path.

**Trailing bits: writers must write zero; readers must not look; tools may
judge.** *(Adopted 2026-08-15; the ruling verbatim: "Yes, I am OK with
writers must write zero, readers must ignore non-zero. And it's good for a
check, we want to check-- was this really written by serialize? and this is
another way to encode this in.")* After the final operation of a message, up
to 7 bits may remain in the final byte. Three rules, one per party:

* Writers **must emit zero** in the unused bits of the final byte. Every
  implementation already does this by construction — the flushed scratch
  beyond the bit index is zero — and the behavior is now an obligation
  rather than an observation. It makes the encoding canonical: among
  conforming writers, one logical stream is exactly one byte sequence.
* Readers **must not reject** a stream for the contents of those bits. No
  read operation examines them. The zero-check obligation applies exactly
  where an operation actually reads padding: `serialize_align`, and the
  alignment step inside `serialize_bytes` and `serialize_string`.
* Non-zero trailing bits are a **provenance signal, not a protocol error**:
  a conformance or diagnostic check may treat them as evidence that the
  stream was not produced by a conforming writer — another way to ask "was
  this really written by serialize?". Such a check lives in tooling and
  validators, never on the read path, and its verdict never changes what a
  reader accepts.

**Refusal rules are part of the format.** The per-operation obligations stated
above — decoded values within `[min,max]`, decoded offsets within range,
alignment padding zero, `wstring` groups at or below `0xFFFF` — are refusal
rules, not advice. An implementation that skips one accepts streams a
conforming implementation refuses, and two implementations that disagree about
refusal disagree about the format. Every refusal rule is
testable by a vector that a conforming reader must reject.

## Implementation Law

*(Adopted 2026-08-16, after a six-implementation audit found invented contracts
replicating port to port. These rules govern how implementations are built, not
just what bytes they emit — because the audit proved the bytes stay honest only
when the practice does.)*

**The job.** When this library is ported to a language, the job is to find
**the fastest correct implementation in that language.** Correct is defined by
this standard; fastest is defined by measurement. Everything below serves that
sentence.

**Sources.** An implementation derives from exactly two sources: this standard,
and — where the standard is silent — the C++ implementation
(`mas-bandwidth/serialize`), which breaks the tie under the authority rule in
Provenance. **Sibling ports are never sources.** Copying a sibling's behavior
because it is the nearest working example is how inventions travel disguised as
specification; every port-to-port inheritance in the audit was carrying one. If
the standard lacks the information needed to implement correctly and fast,
**the standard is too loose: tighten it here, upstream — never improvise in a
port.** Every sentence that sends a reader to the C++ implementation for a rule
is owed a rule in this document and a vector in the corpus, and what those
sentences still cover is listed as the remaining silences under Provenance.

**The check model.** The caller is responsible for well-formed writes. Where
the language has checked builds, write-side contract validation uses them and
nothing else; release builds perform **zero** write-side validation in C and
C++, and the minimum the language permits elsewhere. Readers perform
exactly the refusal obligations of this standard (see Reader Obligations) plus
buffer-end reporting — and nothing more. A check that neither this standard
mandates nor the language forces is an invented contract, whatever its
justification sounds like: "safer", "more defensive", and "best practice" are
the exact phrases the audit found attached to every invention.

A write-side assertion inspects the caller's value as the caller passed it,
before any narrowing the operation performs on the way to the wire. An
assertion placed after the narrowing sees a value already truncated to the
operation's width, so it cannot report the out-of-range input it exists to
diagnose.

**The buffer contract.** Reading whole words through the end of the buffer,
with the allocation aligned up so the final word load is legal, is accepted
best practice — the C++ implementation does this, and implementations
should. Machinery that avoids the slack requirement at the cost of
per-operation work in the hot path is a slower correct option, and is refused
by the speed rule below.

**Speed is normative.** Among correct implementations of an operation, the
fastest correct option is the conforming one. A new approach a port invents is
welcome **provided it is the fastest correct option** — beating the C++
implementation is a contribution, and it should then adopt it. The named error
is choosing a slower correct option and calling it good: a port that is slower
than the C++ implementation for any reason other than a documented language
necessity is defective, and the deviation and its necessity must be documented
where the divergence lives. Performance parity is part of conformance in
spirit: C and C++ at total parity; systems languages within a few percent,
with every residual attributed to a named language mechanism. Swift is a
systems language for that parity class, and ARC traffic and bounds checks are
nameable residuals once they are measured and attributed.

## Compatibility Notes

* **The format is not self-describing.** There are no tags, lengths, or type
  markers beyond what the operations imply. A stream is meaningless without the
  exact sequence of calls that produced it. This is the source of its
  compactness and the reason both endpoints must ship compatible code.
* **Ranges are part of the format.** Changing a `min`/`max` on one side changes
  the bit width and silently desynchronizes everything after it. Range changes
  are breaking changes.
* **Alignment is part of the format.** `serialize_bytes` and `serialize_string`
  align; `serialize_bits`, `serialize_int`, `serialize_fixed`,
  `serialize_uint128` and `serialize_wstring` do not.
* **Zero-bit fields are legal.** `min == max` writes nothing at all.

## Provenance

Written 2026-07-21 by Rowan, by reading the then-existing implementation and
verifying every claim against its golden test vector.

**This document is the authority. Where this document and any implementation
disagree, the implementation is a bug.** Where this document is silent, the
behavior of the C++ implementation (`mas-bandwidth/serialize`) breaks the tie,
and it holds that standing only until this document is amended to state the
rule itself. That tie-break is a marker over a silence rather than a source:
what it still covers is listed as the remaining silences below, each owed a
rule here and a vector in the corpus, and this sentence goes when that list is
empty. A port never copies an implementation over the text of this
document. `serialize.h` is one implementation among nine: C, C++, C#, Dart,
Elixir, Go, Java, JavaScript and Rust. It was the first, which is a fact about
history and not about standing.

**A rule this document states binds every implementation, including the ones
that do not have it yet.** This document leads. Where an implementation lags a
rule, its repository names the gap and the release that closes it, and the gap
is a defect in that implementation rather than a reading of this text.

Until 2026-08-14 this section said the opposite, and the cost of that sentence
was measurable. Under it, five implementations quietly disagreed about
`compressed_float`'s precision and each was, by this document's own terms,
correct: C quantized in `double`, C++ and Go contracted the multiply and add
into a single fused multiply-add on arm64, C# and Rust rounded twice in
`float32`. Four different byte streams from one paragraph, and no divergence
was formally a defect, because whatever an implementation did was by definition
the format.

Every normative statement here is testable, by a pinned vector or by an
explicit refusal test. An implementation conforms when it reproduces every
vector byte for byte and refuses everything this document says must be refused.

**The shared corpus is the conformance instrument.** It is the `conformance/`
directory of this repository, one file per operation, holding the accepted and
refused vectors this document's rules require. Every implementation vendors and
syncs that directory the way it vendors this document, and its test suite must
run every vector in it. No checker reimplements the codec and then checks that
reimplementation against itself. A suite that regenerates its own expectations
proves only that a port agrees with itself, which is how one wrong reading of
this document travels to nine implementations under green results.

**The remaining silences.** These are the rules a reader can get today only by
reading an implementation, each named by the section that owes it and by what a
vector would pin. The tie-break sentence above stands until this list is empty.

| section | what a vector would pin |
|---|---|
| `bits`, `bool`, `uint128` | the 32-bit group split above 32 bits, and the low half first at 128 |
| `align` and `bytes` | non-zero padding refused, and a zero count that still aligns |
| `int` and `int64` | a decoded value outside `[min,max]` refused, and a degenerate range that consumes nothing |
| `fixed` | a negative tie value, which separates half away from zero from an arithmetic shift |
| `float` and `double` | NaN payloads, a signaling NaN, negative zero and a denormal, compared as bit patterns |
| `compressed_float` | the clamp witnesses `[0, 8388609]` and `[0, 16777215]` at resolution `1`, the fusion witness `8388608.0` in that band, and a non-zero `min` decode pinned bit exactly |
| `object` | a nested sequence, which adds no bytes of its own |
| `string` | valid UTF-8 accepted, and invalid UTF-8, an interior NUL and an out-of-range length refused |
| `wstring` | a surrogate pair accepted, and an unpaired surrogate, a group above `0xFFFF` and a zero group refused |
| The Measure Stream | `measure >= bits written` over a sequence vector, at every starting bit position |
| Reader Obligations | a read past the end refused, and the next read on that stream refused too |
| Worked Example | the 112-byte golden message as a sequence vector, with its operation sequence |

**The vector format.** A vector file is text. `#` begins a comment, blank lines
separate records, and each record is `key` and value, one per line:

| key | meaning |
|---|---|
| `operation` | the operation under test, once per record |
| `name` | a stable identifier for the vector |
| `param` | one parameter as `name = value`, repeated once per parameter |
| `bytes` | the stream, as hexadecimal byte pairs, empty for a zero-bit read |
| `expect` | the word `refused`, or `value = ` and the decoded value, or `bits = 0x` and the decoded bit pattern |
| `consumed` | bits a conforming reader consumes, accepted reads only |
| `writer` | the word `canonical`, on a vector that also pins the bytes a writer emits |

`consumed` is stated only for accepted reads. **After a refusal the stream
position is not part of the contract**, so no vector states it and no
implementation is judged on it.

**A vector binds the reader only, unless it carries `writer = canonical`.** An
accepted vector states what a reader must decode from those bytes and states
nothing about what a writer emits for that value, which is what lets a vector
pin a valid non-canonical encoding such as an `int_relative` absolute tier
carrying a difference a writer would have sent in a narrower tier. A vector
carrying `writer = canonical` additionally pins the bytes a conforming writer
produces for that value.

**Values are typed by the operation's table.** A `param` takes the type its own
section gives that parameter, so `min` and `max` under `fixed` are whole real
units, `buffer_size` under `string` is a byte count, and `res` under
`compressed_float` is a `float32`.

**Lexical rules.** `#` begins a comment at the start of a line and nowhere
else, numbers are written as signed decimal or as `0x` hexadecimal, and a
parser must accept values up to 128 bits wide. `expect bits = 0x...` is the
spelling for a value compared as a bit pattern rather than as a number, which
is how `float` and `double` vectors are stated.

**A harness presents every stream with the slack the contract requires.** The
bytes of a vector are the stream, and a harness running it against an
implementation under the eight-byte slack contract allocates that slack behind
them, including for the empty stream of a zero-bit read.

**Conformance vectors must discriminate.** A value taken from the middle of a
range, or one that lands where every plausible reading agrees, proves nothing —
the `compressed_float` divergence above survived years of green test suites
because every pinned value in every implementation landed exactly on a quantum,
where `float32`, `double` and a fused multiply-add all produce the same answer.
A vector that cannot fail is not evidence.
