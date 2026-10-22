# serialize

This document specifies the **wire format** produced and consumed by the
serialize library, precisely enough to write an independent implementation that
interoperates byte for byte.

It describes a format, not an implementation. Nothing here constrains how you
structure your code. [IMPLEMENTING.md](IMPLEMENTING.md) carries the practice the
family follows when it builds one, which no stream can observe and this document
therefore does not state.

## Format version

**The format version is 2.0.** It names the wire, not a library release. Two
endpoints interoperate when they run releases carrying the same format version,
and a release states which format version it implements.
[COMPATIBILITY.md](COMPATIBILITY.md) lists the release of every implementation
in the family that carries it, pinned to the commit of this document and of the
corpus that those releases vendor.

The principle 2.0 adopts *(Glenn Fiedler, 2026-10-22)*: **the wire carries only
integers and bytes, and every value has exactly one encoding.** A reader
refuses anything a conforming writer cannot produce, so the same value is always
the same bytes. [vectors: message-golden-whole, align-refuse-one-non-zero-pad-bit, end-refuse-zero-byte-after-an-unaligned-object, end-refuse-lowest-padding-bit, absolute-refuse-non-canonical-difference-of-one, float-refuse-quiet-nan, int-range-0-8-refuse-offset-one-past-the-span]

**What 2.0 changed, and what it breaks.** Every finite value that 1.1 could
carry keeps its exact bytes. The changes are refusals, one removal, and one new
operation:

* **`wstring` is removed.** The operation, its section, its vectors and its
  step spelling are gone. Text travels as `string`, which is UTF-8. A 1.1
  stream that carries a `wstring` field has no 2.0 reading, and code that calls
  it does not compile against a 2.0 release. A C++ program that holds wide text
  converts at its own boundary with `serialize_utf.h`, which this repository
  ships beside the library and which is not part of the format.
* **`float` and `double` carry finite values only.** NaN, with any payload or
  sign, and both infinities are refused on read as `non_finite_float`. Negative
  zero and the subnormals are finite and keep their exact bits. A 1.1 stream
  carrying a non-finite pattern is refused by a 2.0 reader. [vectors: float-refuse-quiet-nan, double-refuse-positive-infinity, float-negative-zero, double-smallest-positive-denormal]
* **`compressed_float` refuses a non-finite decode.** A declaration whose top
  decodes to an infinity is now non-conforming as a whole, so its writer asserts
  in checked builds even for values that decode finite; its bytes in release
  are unchanged. [vectors: compressed-float-refuse-decode-rounding-to-infinity]
* **`int_relative` refuses a non-canonical tier.** The absolute tier carrying a
  difference a bounded tier would have taken is refused as `non_canonical`. [vectors: absolute-refuse-non-canonical-difference-of-one, absolute-refuse-non-canonical-last-bounded-difference]
* **`end` is new.** A reader that has read the last field of an object reads
  its end, which refuses a whole byte after the object as `trailing_data` and a
  set bit in the final byte's padding as `nonzero_padding`. Under 1.1 those bits
  were never examined. [vectors: end-refuse-one-zero-byte-after-the-empty-object, end-refuse-highest-padding-bit]
* **Every refusal has a reason**, named from the closed list under "Refusal
  reasons", and every refused vector states the one it expects. [controls: wrong-reason]
* **Wide integers are exact.** The document now states it, and the corpus pins
  the edges where an implementation backed by a double loses bits. [controls: int64-through-double]

Format version 1.1 was the rulings of 2026-08-15, 2026-08-23 and 2026-09-04 over
the document as first written (1.0); every one of them stands in 2.0.

## How to read this document

A paragraph that states a rule uses **must**, **refuse** or **required**, and it
ends with the vectors that prove it, a square bracketed `vectors:` list naming
records in `conformance/`, or the controls that prove it, a `controls:` list
naming files in `conformance-controls/`. A rule no vector could
fail is not stated here. The citation covers every sentence of its paragraph,
and `tools/conformance` fails the build when a paragraph that states a rule
cites nothing, when it cites a name the corpus does not hold, and when a vector
is cited by no rule. [controls: uncited-rule]

Every other paragraph explains, and binds nothing on its own.

Every feature this document defines is required of every implementation in the
family. There are no optional operations, no optional refusals and no
implementation latitude about what a stream means: a runner that meets an
operation it has no code for fails. [controls: unknown-operation]

## Architecture

serialize is a **bit packer**. Values are written as variable numbers of bits
rather than whole bytes, so a boolean costs one bit and an integer known to lie
in `[0,7]` costs three.

Reading and writing are expressed once, as a single templated function per
message type, instantiated against a write stream, a read stream, or a measure
stream. The measure stream computes a conservative bound on the size a message
would occupy, without producing bytes. Everything else here concerns the bytes
on the wire.

## General Conventions

All multi-byte quantities are **little-endian**.

The stream is accumulated in a **64-bit scratch word**. Bits are packed
**least-significant-bit first**: the first value written occupies the lowest
bits of the first word. When the scratch fills, the 64-bit word is copied to the
buffer in little-endian byte order. The result is the same bytes on the wire on
every host.

A value of `n` bits, written when the scratch already holds `s` bits, occupies
bits `[s, s+n)` of the current word. A value that would cross the 64-bit
boundary is split: the low `64-s` bits complete the current word, and the
remainder begins the next.

**Flush and length.** After the final value, any partially filled scratch word
is written out. The meaningful length of a stream is the number of bytes its
bits occupy, rounded up to a whole byte, and that is the stream a writer hands
on: the bytes past it in the writer's buffer are not part of it.

A writer must emit zero in the unused bits of the final byte, so one object has
one byte sequence. [vectors: message-golden-whole, end-accept-after-three-bits, end-accept-seven-bits-of-padding, bits-1-one]

**Bit index and alignment.** The bit index is the count of bits written or read
so far. The stream is *aligned* when the bit index is a multiple of 8. The
number of bits needed to reach alignment is `(8 - (bit_index % 8)) % 8`.

**Writers are trusted; readers are not.** The write path is the caller's: a
caller hands a writer values inside their declarations, and this document owes
a conforming writer exact bytes and owes a misbehaving writer nothing. The read
path faces untrusted bytes, and every reader rule below binds in every build of
every implementation. How an implementation reports a writer's misuse is
practice, recorded in IMPLEMENTING.md, because no stream can show it.

## Bit-Level Primitives

### bits

    serialize_bits( stream, value, bits )

Writes the low `bits` bits of `value`, where `bits` is in `[1,64]`.

* For `bits <= 32` this is a single group of that many bits.
* For `bits > 32` the value is split: the **low 32 bits are written first as a
  32-bit group**, then the remaining `bits - 32` high bits as a second group.

A reader must decode the groups in that order and must reproduce every width
from 1 to 64 exactly. [vectors: bits-1-zero, bits-1-one, bits-8-max, bits-24, bits-32-max, bits-33-low-group-precedes-high-group, bits-33-max, bits-64-high-half-only, bits-64-low-half-only, bits-64-max]

Fixed-width helpers are aliases for exactly this, and carry no range
information of their own. They are part of every implementation's surface,
because they name the native unsigned types:

| helper | equivalent to |
|---|---|
| `serialize_uint8( value )`  | `serialize_bits( value, 8 )` |
| `serialize_uint16( value )` | `serialize_bits( value, 16 )` |
| `serialize_uint32( value )` | `serialize_bits( value, 32 )` |
| `serialize_uint64( value )` | `serialize_bits( value, 64 )`, low 32 then high 32 |

The aliases must produce the bytes of the operation they name. [vectors: bits-8-alias-uint8, bits-16-alias-uint16, bits-32-alias-uint32, bits-64-alias-uint64]

A field read looks at its own bits and no others: the bits after a field belong
to whatever reads next, and only an `end` examines the unused bits of a final
byte. A reader must accept a one-bit field whose byte has its other bits set.
[vectors: bits-1-reader-ignores-trailing-bits, bool-true-reader-ignores-trailing-bits, bool-false-with-higher-bits-set]

### bool

    serialize_bool( stream, value )

One bit: `1` for true, `0` for false. A reader must decode exactly that bit.
[vectors: bool-false, bool-true]

### uint128

    serialize_uint128( stream, value )

A 128-bit unsigned integer, always 128 bits on the wire: the **low 64-bit half
first**, then the high half, each half written exactly as
`serialize_bits( half, 64 )`. When the stream is byte aligned, the result is the
16 bytes of the value in little-endian order.

A reader must decode the halves in that order and carry all 128 bits exactly.
[vectors: uint128-zero, uint128-low-half-only, uint128-high-half-only, uint128-distinct-halves, uint128-max]

### align

    serialize_align( stream )

Pads with **zero bits** until the bit index is a multiple of 8. If the stream
is already aligned, **nothing is written**.

A writer must pad with zero bits and nothing else, and write nothing when the
stream is already aligned. [vectors: align-no-op-when-already-aligned, align-no-op-at-bit-index-zero, align-pads-one-bit, align-pads-four-bits, align-pads-seven-bits, sequence-align-pads-after-an-odd-width, sequence-second-align-is-a-no-op]

A reader must refuse an align whose padding holds a set bit, as
`nonzero_padding`. [vectors: align-refuse-one-non-zero-pad-bit, align-refuse-four-non-zero-pad-bits, align-refuse-seven-non-zero-pad-bits, align-refuse-only-the-highest-pad-bit-set, align-refuse-only-the-lowest-pad-bit-set, sequence-refusal-is-terminal-after-bad-alignment] [controls: align-padding]

## Integers

The integer operations stay distinct on purpose: `bits` and its aliases, `int`,
`int64`, `int128` and `fixed` map to the native integer types of C, C++, Rust and
Go, and that mapping is where the format's speed comes from.

### int (ranged)

    serialize_int( stream, value, min, max )

The defining operation of the format. The number of bits used is determined
entirely by the range:

    bits_required( min, max ) = ( min == max ) ? 0 : 32 - count_leading_zeros( max - min )

The arithmetic is unsigned and wrapping: `max - min` and `value - min` are
computed in the unsigned domain of the operation's width, `uint32` for `int`,
`uint64` for `int64` and `uint128` for `int128`, and the leading zero count is
taken at that same width, so a range as wide as `[INT32_MIN, INT32_MAX]` is
exact rather than overflowing. `bits_required64` and `bits_required128` are the
same rule at 64 and 128 bits.

`value - min` is written in that many bits, so a range of `[0,7]` costs 3 bits
and `[0,8]` costs 4. A writer must use exactly `bits_required( min, max )` bits.
[vectors: int-range-0-7-costs-three-bits, int-range-0-7-accepts-its-maximum, int-range-0-8-costs-four-bits, int-range-0-2-accepts-its-maximum, int-negative-min-accepts-its-minimum, int-negative-min-accepts-its-maximum, int-range-0-1-single-bit-zero, int-range-0-1-single-bit-one, int-negative-min-golden-field, int-full-int32-range-golden-field, int-full-int32-range-minimum, int-full-int32-range-maximum, int-span-31-bits-accepts-its-maximum] [controls: ranged-reconstruction]

`min <= max` is the legal relation for every ranged operation: `int`, `int64`,
`int128` and `fixed`. A **degenerate range** where `min == max` costs **zero
bits**: the writer emits nothing, and a reader must consume nothing and take the
value from `min`. [vectors: int-degenerate-range-zero-bits, int-degenerate-range-negative, int-degenerate-range-at-int32-min, int-degenerate-range-accepts-the-empty-stream, int64-degenerate-range-zero-bits, int64-degenerate-range-at-int64-min, degenerate-range-zero-bits, int128-degenerate-range-negative, sequence-zero-bit-field-between-wide-ones]

A reader must refuse an offset greater than `max - min`, compared in the
unsigned domain before the value is formed, as `value_out_of_range`: reject,
never clamp. [vectors: int-range-0-8-refuse-offset-one-past-the-span, int-range-0-8-refuse-offset-fifteen, int-range-0-2-refuse-offset-three, int-negative-min-refuse-offset-one-past-the-span, int-negative-min-refuse-offset-all-ones, int-span-31-bits-refuse-offset-one-past-the-span, sequence-refusal-is-terminal-after-a-range-refusal] [controls: ranged-range]

The range is identical on both sides. The format carries no self-description: a
stream is only interpretable by a reader that performs the same sequence of
operations with the same parameters.

### int64 (ranged)

    serialize_int64( stream, value, min, max )

The 64-bit counterpart of the ranged integer, and the only ranged 64-bit
operation. `bits_required64( min, max )` bits are used. If that is 32 or fewer,
the value is a single group of that many bits; otherwise the low 32 bits come
first, followed by the remaining `bits - 32` high bits.

A reader must decode the groups in that order and refuse an offset past the
span as `value_out_of_range`. [vectors: int64-narrow-range-matches-int, int64-single-bit-range, int64-span-exactly-32-bits-single-group, int64-span-exactly-32-bits-accepts-its-maximum, int64-span-33-bits-low-group-precedes-high-group, int64-span-33-bits-accepts-its-full-width, int64-span-42-bits-negative-bounds, int64-span-42-bits-accepts-its-minimum, int64-span-42-bits-accepts-its-maximum, int64-full-range-zero, int64-full-range-minimum, int64-full-range-maximum, int64-span-64-bits-accepts-its-maximum, int64-span-33-bits-refuse-offset-one-past-the-span, int64-span-42-bits-refuse-offset-one-past-the-span, int64-span-64-bits-refuse-offset-one-past-the-span]

`serialize_uint64` is not ranged: it is `serialize_bits( value, 64 )` and always
costs 64 bits. The names are similar and the encodings are not.

### int128 (ranged)

    serialize_int128( stream, value, min, max )

The 128-bit counterpart, and the only ranged 128-bit operation.
`bits_required128( min, max )` bits are used, with `min` and `max` converted to
the unsigned 128-bit domain first, so a range wider than `2^127` is exact. The
offset is written in 32-bit groups from least significant upward: `bits <= 32`
is a single group, otherwise full 32-bit groups from the bottom with the final
group carrying the remainder, up to four groups. Where the range fits 64 bits or
fewer the bytes are identical to `serialize_int64( value, min, max )`.

A reader must decode the groups in that order and refuse an offset past the
span as `value_out_of_range`. [vectors: int128-narrow-range-matches-int64, int128-64-bit-span-matches-int64, int128-72-bit-span-three-groups, int128-72-bit-span-accepts-its-minimum, int128-72-bit-span-accepts-offset-equal-to-the-span, int128-full-range-zero, int128-full-range-minimum, int128-full-range-maximum, int128-128-bit-span-accepts-its-maximum, int128-group-boundary-bit-32, int128-group-boundary-bit-64, int128-group-boundary-bit-96, int128-72-bit-span-refuse-offset-one-past-the-span, int128-72-bit-span-refuse-offset-all-ones, int128-128-bit-span-refuse-offset-one-past-the-span]

`serialize_uint128` is not ranged: it always costs 128 bits.

### Wide integers are exact

A 64-bit or 128-bit value is an integer on the wire, and every implementation
must carry it as an integer, bit for bit, from the bytes to the caller and back.
A runtime whose ordinary number is a double carries the value in an integer
type, a big integer, or a pair of exact halves, because a double holds every
integer up to `2^53` and loses the rest: `2^53 + 1` rounds to `2^53`, `2^63 - 1`
rounds to `2^63`, and `2^64 - 1` rounds past the width. The corpus pins the
edges around each limit, because a power of two survives a double and its
neighbours do not. [vectors: bits-64-exact-2p53-minus-1, bits-64-exact-2p53, bits-64-exact-2p53-plus-1, bits-64-exact-2p63-minus-1, bits-64-exact-2p63, bits-64-exact-2p64-minus-1, int64-full-range-exact-2p53-minus-1, int64-full-range-exact-2p53, int64-full-range-exact-2p53-plus-1, int64-full-range-exact-minus-2p53-minus-1, int64-full-range-exact-2p63-minus-2, int64-full-range-exact-minus-2p63-plus-1, uint128-exact-2p53-plus-1, uint128-exact-2p64-minus-1, uint128-exact-2p64, uint128-exact-2p64-plus-1, uint128-exact-2p127, uint128-exact-2p128-minus-2, int128-full-range-exact-2p53-plus-1, int128-full-range-exact-2p63, int128-full-range-exact-2p64-plus-1, int128-full-range-exact-minus-2p64-minus-1, int128-full-range-exact-2p127-minus-2, int128-full-range-exact-minus-2p127-plus-1] [controls: int64-through-double]

### fixed (Q format, ranged)

    serialize_fixed( stream, value, integer_bits, fraction_bits, min, max )

A fixed point value held in a signed integer storage type of exactly
`integer_bits + fraction_bits` bits, 8, 16, 32, 64 or 128, with the sign bit
counting toward `integer_bits` (Q48.16 in an `int64_t`, Q112.16 in a 128-bit
integer). The stored integer is the real value scaled by `2^fraction_bits`.
`min` and `max` are bounds in **whole real units**, and all four parameters are
part of the format, exactly like a ranged integer's bounds. A language whose
generics cannot take integer parameters passes them at runtime and produces the
same bytes. `value` is the raw scaled integer, and the operation performs no
conversion of its own.

The encoding is an offset over the **raw** bounds, in the unsigned domain of
the storage width:

    raw_min = min << fraction_bits
    raw_max = max << fraction_bits
    bits    = bit length of ( raw_max - raw_min )

`raw_value - raw_min` is written in `bits` bits, in 32-bit groups from least
significant upward exactly as `serialize_bits` splits wide values. For storage
of 64 bits or fewer the bytes are identical to
`serialize_int64( raw_value, raw_min, raw_max )`, and with `fraction_bits = 0`
the operation is a ranged integer.

A reader must decode the groups in that order and refuse an offset past
`raw_max - raw_min` as `value_out_of_range`. [vectors: fixed-q8-8-worked-example, fixed-q8-8-accepts-its-raw-minimum, fixed-q8-8-accepts-offset-equal-to-the-raw-span, fixed-zero-fraction-bits-is-a-ranged-int, fixed-zero-fraction-bits-accepts-its-maximum, fixed-q16-16-raw-span-32-bits, fixed-q16-16-accepts-offset-equal-to-the-raw-span, fixed-q48-16-raw-span-34-bits-two-groups, fixed-q48-16-accepts-offset-equal-to-the-raw-span, fixed-q112-16-raw-span-75-bits-group-one, fixed-q112-16-raw-span-75-bits-group-two, fixed-q112-16-accepts-offset-equal-to-the-raw-span, fixed-q112-16-negative-raw-minimum-decodes-zero, fixed-q112-16-negative-raw-minimum-accepts-its-minimum, fixed-q64-64-raw-span-127-bits-group-one, fixed-q64-64-raw-span-127-bits-group-two, fixed-q64-64-raw-span-127-bits-group-three, fixed-q64-64-accepts-offset-equal-to-the-raw-span, fixed-q8-8-refuse-offset-one-past-the-raw-span, fixed-q8-8-refuse-offset-all-ones, fixed-zero-fraction-bits-refuse-offset-one-past-the-span, fixed-q16-16-refuse-offset-one-past-the-raw-span, fixed-q16-16-refuse-offset-all-ones, fixed-q48-16-refuse-offset-one-past-the-raw-span, fixed-q112-16-refuse-offset-one-past-the-raw-span, fixed-q112-16-refuse-offset-all-ones, fixed-q64-64-refuse-offset-one-past-the-raw-span, fixed-q64-64-refuse-offset-all-ones]

A degenerate range costs zero bits on every storage width: a reader must
consume nothing and take the raw value `min << fraction_bits`, on a Q112.16 or
Q64.64 field exactly as on a Q16.16 one. [vectors: fixed-degenerate-range-q16-16, fixed-degenerate-range-q112-16-negative, fixed-degenerate-range-q64-64-zero, fixed-degenerate-range-q64-64-nonzero]

Fixed point values are integers underneath, so the round trip is exact: the
same raw value is the same bytes and reads back bit for bit identical on every
platform. How a runtime rounds when it converts between floating point and a Q
format is outside the wire and recorded in IMPLEMENTING.md.

### int_relative

    serialize_int_relative( stream, previous, current )

Encodes an increasing sequence compactly, where `current > previous`. Let
`difference = current - previous`. The encoding is a ladder of one-bit flags,
each answering "does it fit in this tier?":

| tier | flag sequence | payload | difference range |
|---|---|---|---|
| `one-bit` | `1` | none | exactly 1 |
| `bounded-3` | `0 1` | `serialize_int( d, 2, 6 )`, 3 bits | 2 to 6 |
| `bounded-5` | `0 0 1` | `serialize_int( d, 7, 23 )`, 5 bits | 7 to 23 |
| `bounded-9` | `0 0 0 1` | `serialize_int( d, 24, 280 )`, 9 bits | 24 to 280 |
| `bounded-13` | `0 0 0 0 1` | `serialize_int( d, 281, 4377 )`, 13 bits | 281 to 4377 |
| `bounded-17` | `0 0 0 0 0 1` | `serialize_int( d, 4378, 69914 )`, 17 bits | 4378 to 69914 |
| `absolute` | `0 0 0 0 0 0` | `current` as 32 raw bits | above 69914 |

**The writer takes the first tier the difference fits**, and that is the only
encoding of the value. The final tier transmits `current` itself rather than
the difference, as an unsigned 32-bit group. A difference of 1, the common case
for sequence numbers, costs a single bit. A writer must use the first tier that
fits. [vectors: one-bit-accept, bounded-3-accept-low, bounded-3-accept-high, bounded-5-accept-low, bounded-5-accept-high, bounded-9-accept-low, bounded-9-accept-high, bounded-13-accept-low, bounded-13-accept-high, bounded-17-accept-low, bounded-17-accept-high, absolute-accept-first-difference-past-the-last-tier, absolute-accept-domain-maximum, absolute-accept-domain-maximum-from-the-last-tier-boundary]

**The domain is `0` to `2^31 - 1` inclusive**, for `previous` and `current`
alike, and there is no wrapping: a caller with a wrapping counter unwraps it
before serializing. `previous` is the caller's own state and never arrives off
the wire. A reader must reconstruct `current` in a width that cannot wrap, and
must refuse the read as `value_out_of_range` unless `current` lies in the domain
and is strictly greater than `previous`, in every tier. [vectors: one-bit-refuse-past-domain, one-bit-accept-at-domain-top, bounded-3-refuse-past-domain, bounded-3-accept-at-domain-top, bounded-5-refuse-past-domain, bounded-5-accept-at-domain-top, bounded-9-refuse-past-domain, bounded-9-accept-at-domain-top, bounded-13-refuse-past-domain, bounded-13-accept-at-domain-top, bounded-17-refuse-past-domain, bounded-17-accept-at-domain-top, absolute-refuse-not-increasing, absolute-refuse-current-equal-to-previous, absolute-refuse-current-below-previous, absolute-refuse-top-bit-set, absolute-refuse-all-bits-set]

Each bounded tier's payload is a ranged integer, and a reader must refuse a
payload offset past its span as `value_out_of_range`. [vectors: bounded-3-refuse-payload-one-past-the-span, bounded-3-refuse-payload-all-three-bits, bounded-5-refuse-payload-one-past-the-span, bounded-5-refuse-payload-all-five-bits, bounded-9-refuse-payload-one-past-the-span, bounded-9-refuse-payload-all-nine-bits, bounded-13-refuse-payload-one-past-the-span, bounded-13-refuse-payload-all-thirteen-bits, bounded-17-refuse-payload-one-past-the-span, bounded-17-refuse-payload-all-seventeen-bits]

**The absolute tier is canonical too.** A reader must refuse the absolute tier
carrying a reconstructed `current` that is in the domain, above `previous`, and
no more than 69914 above it, as `non_canonical`: that value has a bounded
encoding, and the absolute one is a second encoding of it. The domain and
ordering rule comes first, so a `current` outside the domain is
`value_out_of_range` whatever its difference. The bounded tiers need no such
rule, because each one's payload range starts one past the tier before it.
[vectors: absolute-refuse-past-domain-with-a-small-difference, absolute-refuse-non-canonical-difference-of-one, absolute-refuse-non-canonical-last-bounded-difference, absolute-accept-domain-maximum-from-the-last-tier-boundary] [controls: int-relative-canonical]

## Floating Point

### float

    serialize_float( stream, value )

The 32 bits of the IEEE-754 single-precision representation, written as a
32-bit group. No conversion, no compression.

### double

    serialize_double( stream, value )

The 64 bits of the IEEE-754 double-precision representation, written as one
64-bit group: the low 32-bit group first, as `serialize_bits` over 64 bits.

### Finite values only, every one bit exact

*(Adopted 2026-10-22; Glenn Fiedler: "What if we just took all the non-correct
numbers for float/double and just disallowed sending them across all langs?
infs, nans and so on", then "Good. Let's do this".)*

`float` and `double` carry **finite** values. A value whose exponent field is
all ones (NaN with any payload, either sign, signaling or quiet, and both
infinities) is not a value the format carries. A writer is handed finite values
only, as it is handed an `int` inside its range. A reader must refuse a
non-finite pattern as `non_finite_float`, and leave its destination unwritten.
[vectors: float-refuse-positive-infinity, float-refuse-negative-infinity, float-refuse-quiet-nan, float-refuse-negative-quiet-nan, float-refuse-signaling-nan, float-refuse-nan-with-payload, float-refuse-negative-nan-all-payload-bits, float-refuse-nan-all-payload-bits, double-refuse-positive-infinity, double-refuse-negative-infinity, double-refuse-quiet-nan, double-refuse-negative-quiet-nan, double-refuse-signaling-nan, double-refuse-nan-with-payload, double-refuse-negative-nan-all-payload-bits, double-refuse-nan-payload-in-low-group-only] [controls: float-finite, double-finite]

Every finite pattern is accepted and reproduced exactly. Negative zero and the
subnormals are finite: a reader must not flush, canonicalize or re-sign them,
and the bits a reader returns must be the bits it read. Flush to zero is an
arithmetic concern of the program that uses the value, never of storage.
[vectors: float-positive-zero, float-negative-zero, float-one, float-negative-one, float-pi, float-smallest-positive-denormal, float-largest-denormal, float-negative-denormal, float-largest-negative-denormal, float-smallest-positive-normal, float-largest-finite, float-most-negative-finite, double-positive-zero, double-negative-zero, double-one, double-one-third, double-low-half-only, double-high-half-only, double-smallest-positive-denormal, double-largest-denormal, double-negative-denormal, double-smallest-positive-normal, double-largest-finite, double-most-negative-finite, sequence-float-unaligned-between-bools]

Conformance vectors for these operations compare **bit patterns, not values**:
`-0.0 == 0.0` in value space, and a tolerance comparison cannot see a flushed
subnormal.

### compressed_float

    serialize_compressed_float( stream, value, min, max, res )

A float quantized to a resolution. Let `delta = max - min` and
`values = delta / res`, clamped to `[1, 4294967040]` (the largest float below
`2^32`). Then:

    max_integer_value = ceil( values )
    bits              = bits_required( 0, max_integer_value )

The writer clamps `(value - min) / delta` to `[0,1]`, multiplies by
`max_integer_value`, adds `0.5`, takes the floor, **clamps the resulting integer
to `max_integer_value`**, and writes it in `bits` bits. The reader divides by
`max_integer_value`, multiplies by `delta`, and adds `min`.

A declaration takes `min < max` and `res > 0`, and `delta`, `values` and the
decode of `max_integer_value` (`min + delta`, rounded once) finite in
`float32`. Anything else is a non-conforming declaration with no wire meaning.
The quantized integer is the encoding: one integer, one set of bits.

**The arithmetic is `float32`, and the roundings are part of the format.** On
write, the product `normalized * max_integer_value` rounds to `float32` before
`0.5` is added, and that sum rounds before the floor: two roundings, never
widened to `double` and never fused into one multiply-add. Between quanta those
differences change the bytes: over `[0, 10]` at resolution `0.01`, a conforming
writer writes `1`, `3`, `11` and `1000` for `0.005`, `0.025`, `0.105` and
`9.995`, where a writer that widens to `double` writes `0`, `2`, `10` and `999`.
In the clamp band, `max_integer_value` in `[2^23, 2^24)`, the product's ulp
reaches 1 and a fused writer moves the integer, first at `8388608.0` over
`[0, 16777215]` at resolution `1`. A writer must emit the bytes of the two
rounding arithmetic and the integer clamp. [vectors: compressed-float-writer-input-0p005-over-0-10, compressed-float-writer-input-0p025-over-0-10, compressed-float-writer-input-0p105-over-0-10, compressed-float-writer-input-9p995-over-0-10, compressed-float-writer-input-fusion-witness-2p23, compressed-float-clamp-band-fusion-witness-value, compressed-float-wire-divergence-band-fusion-witness-value, compressed-float-clamp-band-accepts-max-integer-value, compressed-float-wire-divergence-band-accepts-max-integer-value]

The decode is `float32` with every step rounding: the quotient rounds, the
product rounds before `min` is added, and the sum rounds. A reader must decode
to exactly those bits, never widened and never fused. Fused, a decode over a
non-zero `min` lands one ulp away. [vectors: compressed-float-zero-integer-over-0-10, compressed-float-on-quantum-anchor-over-0-10, compressed-float-integer-one-over-0-10, compressed-float-accepts-max-integer-value-over-0-10, compressed-float-nonzero-min-integer-zero, compressed-float-nonzero-min-midpoint, compressed-float-nonzero-min-fused-decode-differs-by-one-ulp, compressed-float-nonzero-min-off-quantum, compressed-float-nonzero-min-accepts-max-integer-value, compressed-float-clamp-band-integer-zero, compressed-float-wire-divergence-band-integer-zero, compressed-float-values-clamped-at-the-high-end, compressed-float-values-clamped-integer-zero, compressed-float-values-clamped-at-the-low-end-zero, compressed-float-values-clamped-at-the-low-end-one]

A reader must refuse an integer greater than `max_integer_value` as
`value_out_of_range`. [vectors: compressed-float-refuse-integer-one-past-max-integer-value, compressed-float-refuse-integer-all-ten-bits, compressed-float-nonzero-min-refuse-integer-one-past-max-integer-value, compressed-float-nonzero-min-refuse-integer-all-fifteen-bits, compressed-float-clamp-band-refuse-integer-one-past-max-integer-value, compressed-float-clamp-band-refuse-integer-all-twenty-four-bits, compressed-float-values-clamped-refuse-integer-one-past-max-integer-value, compressed-float-values-clamped-refuse-integer-all-thirty-two-bits]

**The decode must be finite.** Every operand is finite and the integer is in
range, and still the final add can round past `FLT_MAX`: over
`[3 * 2^103, FLT_MAX]` at resolution `2^104`, `delta` rounds up to
`2^128 - 2^105`, and the top integer decodes to `2^128`, which is `+inf`. That
declaration is non-conforming, and a reader must refuse a non-finite decode as
`non_finite_float` whatever the declaration, so no declaration can hand NaN or
an infinity to the far side of the wire. [vectors: compressed-float-refuse-decode-rounding-to-infinity, compressed-float-accept-finite-decode-below-the-top] [controls: compressed-float-finite]

The operation is lossy by construction: a round trip returns the nearest
representable quantum, not the original value.

## object

    serialize_object( stream, object )

Invokes the object's own serialize function inline. It contributes **no bytes
of its own**: whatever the nested object writes appears at exactly this
position, with no framing, length prefix or alignment around it, and a refusal
inside it is a refusal of the whole. A reader must read a nested object exactly
as it reads the same operations unnested. [vectors: object-flat-twin, object-adds-no-bytes-of-its-own, object-nested-in-the-middle-of-a-stream, object-inserts-no-alignment, object-containing-an-aligning-operation, object-refusal-propagates-out-of-the-nesting, object-refusal-past-the-end-propagates]

## Bytes and Strings

### bytes

    serialize_bytes( stream, data, count )

**Aligns first**, then writes `count` raw bytes. `count` is not written: both
sides already agree on it. The alignment is performed when `count` is zero too,
so a zero-length field still pads to the byte boundary. A reader must perform
and verify the same alignment, refusing set padding as `nonzero_padding`,
before it reads the bytes, so set padding is refused as `nonzero_padding` even when the payload would also run past the end. [vectors: bytes-refuse-padding-before-a-truncated-payload, string-refuse-padding-before-a-truncated-payload, bytes-zero-count-aligned-start, bytes-zero-count-still-performs-the-align, bytes-single-byte-aligned-start, bytes-all-zero, bytes-seven-bytes-aligned-start, bytes-after-a-partial-byte, bytes-accept-one-byte-after-the-align, sequence-bytes-realigns-a-mid-byte-stream, bytes-zero-count-refuse-non-zero-align-padding, bytes-refuse-non-zero-align-padding]

### string

    serialize_string( stream, string, buffer_size )

Text, as UTF-8 bytes.

1. The length in bytes, as `serialize_int( length, 0, buffer_size - 1 )`. The
   bit cost depends on `buffer_size`, which both sides agree on, and which
   bounds the length field without describing any destination.
2. The bytes, as `serialize_bytes`, **which aligns**.

No terminator is transmitted. A reader must refuse a length above
`buffer_size - 1` as `value_out_of_range`. [vectors: string-golden, string-empty-still-performs-the-align, string-single-byte, string-fills-the-buffer, string-buffer-size-8-three-bit-length, string-buffer-size-1-zero-bit-length-field, string-accepts-the-longest-length, string-refuse-length-one-past-the-buffer, string-refuse-length-all-four-bits, string-refuse-non-zero-align-padding]

**The payload is Unicode scalar values in well-formed UTF-8, and nothing
else.** A writer is handed well-formed UTF-8 with no zero byte, as it is handed
an `int` in its range. A reader must refuse a payload that is not well formed
under Unicode Table 3-7 as `invalid_utf8`: an overlong form, an encoded
surrogate (`ED A0 80` to `ED BF BF`), a code point above `10FFFF`, a truncated
sequence, a stray continuation byte, or a byte that never appears in UTF-8.
[vectors: string-refuse-overlong-two-byte-nul, string-refuse-overlong-two-byte-slash, string-refuse-overlong-three-byte-slash, string-refuse-surrogate-encoded-in-utf8, string-refuse-low-surrogate-encoded-in-utf8, string-refuse-code-point-above-10FFFF, string-refuse-five-byte-sequence, string-refuse-lone-continuation-byte, string-refuse-continuation-byte-after-ascii, string-refuse-truncated-two-byte-sequence, string-refuse-truncated-three-byte-sequence, string-refuse-byte-fe, string-refuse-byte-ff] [controls: string-utf8, string-utf8-surrogate]

A reader must refuse a zero byte among the transmitted bytes as
`embedded_nul`, before it checks the encoding: a zero byte gives the payload two
lengths, the wire's and the one every consumer that stops at a zero computes,
and whatever sits between them rides past whichever side uses the other. NUL is
well-formed UTF-8, which is why this is its own rule, and a payload that
breaks both rules is refused as `embedded_nul`. [vectors: string-refuse-nul-before-encoding, string-refuse-nul-as-the-only-byte, string-refuse-nul-as-the-first-byte, string-refuse-nul-in-the-middle, string-refuse-nul-as-the-last-byte, string-accept-the-same-three-bytes-without-a-nul] [controls: string-embedded-nul, wrong-reason]

**Validation refuses illegal byte sequences, and never a real character.** A
reader must accept every well-formed payload and hand back exactly the bytes it
read: no normalization (`e` followed by `U+0301` stays three bytes, distinct
from the precomposed `U+00E9`), no reordering of right-to-left text, no
replacement character, no stripped byte order mark, and every plane, four-byte
characters included. Noncharacters such as `U+FFFF` are well formed and travel.
A runtime that decodes into a native string type round trips these bytes
through it unchanged. [vectors: string-accept-shortest-two-byte-sequence, string-accept-shortest-three-byte-sequence, string-accept-just-below-the-surrogate-block, string-accept-just-above-the-surrogate-block, string-accept-astral-code-point, string-accept-the-largest-code-point, string-accept-the-same-two-byte-sequence-completed, string-accept-the-same-three-byte-sequence-completed, string-text-chinese-weiqi, string-text-japanese-go-stones, string-text-japanese-black-and-white, string-text-korean-baduk, string-text-arabic-right-to-left, string-text-combining-acute-not-normalized, string-text-precomposed-e-acute, string-text-emoji-four-byte, string-text-cjk-extension-b-four-byte, string-text-highest-code-point, string-text-byte-order-mark-kept, string-text-noncharacter-kept, string-text-mixed-scripts]

## end

    serialize_end( stream )

The end of the object. **An object ends at the bit index after its last field,
and the stream ends there too**: in the unused bits of that field's final byte,
which a writer leaves zero, and nowhere after. A writer's end emits nothing, and
a measure's costs nothing. A reader reads its end after the last field of the
top-level object it was handed, with the stream holding exactly the bytes the
writer reported, and the end consumes the final byte's padding.

A reader of a packet must read the end after the last field of the packet's
top-level object. The guarantee that one value has one byte sequence holds for
a read that ends this way: without the end, trailing bytes and padding go
unexamined. [vectors: message-golden-whole, end-refuse-zero-byte-after-an-aligned-object, end-refuse-lowest-padding-bit]

A reader's end must refuse a whole byte or more after the object as
`trailing_data`, even a zero byte. Trailing data is checked first: a stream with
a whole byte left over is `trailing_data` whatever its padding holds. [vectors: end-refuse-trailing-data-before-padding, end-accept-empty-stream, end-refuse-one-zero-byte-after-the-empty-object, end-refuse-one-set-byte-after-the-empty-object, end-refuse-zero-byte-after-an-unaligned-object, end-accept-after-an-aligned-object, end-refuse-zero-byte-after-an-aligned-object, end-accept-after-bytes, end-refuse-a-byte-after-bytes] [controls: end-trailing-data]

A reader's end must refuse a set bit among the final byte's unused bits as
`nonzero_padding`, and must not look at the bits that belong to the last field.
[vectors: end-accept-after-three-bits, end-refuse-lowest-padding-bit, end-refuse-highest-padding-bit, end-accept-every-field-bit-set, end-accept-seven-bits-of-padding, end-refuse-seventh-padding-bit] [controls: end-padding]

An end is a read like any other: on a failed stream it is refused with the
first refusal's reason, and no bit can be read after an accepted end, because
nothing is left. A zero-bit read still succeeds there, as it does anywhere a
stream is exactly consumed. [vectors: end-refuse-after-a-refused-field-keeps-the-first-reason, end-then-a-further-read-is-past-the-end, message-golden-whole]

## Worked Example

The library's golden message ends with this run, a string of CJK text and the
first fixed point field after it:

    0x06 0xE7 0xA2 0x81 0xE7 0x9F 0xB3 0xC0 0x60

Decoding it against this document:

* The string is `serialize_string( value, 8 )`, so its length field is
  `serialize_int( length, 0, 7 )`, which is `bits_required(0,7)` = **3 bits**,
  starting on a byte boundary because the field before it was a string too.
* `0x06` is `0000 0110`. Its low 3 bits are `110` = **6**, the length.
* The string's own align pads the other five bits of that byte, which are zero.
* Six bytes follow: `E7 A2 81` is `U+7881` and `E7 9F B3` is `U+77F3`, 碁石,
  go stones.
* The message's align after the string is a no-op: the stream is aligned.
* The next field is `serialize_fixed( value, 8, 8, -100, +100 )`: Q8.8, so
  `raw_min` is `-100 << 8 = -25600`, the raw range is `51200`, and the field
  costs 16 bits. `0xC0 0x60` is the offset `0x60C0 = 24768`, and adding
  `raw_min` gives `-832`, which is `-3.25` in Q8.8.

After another align the message carries two wide fixed point fields that make
the multi-group split load-bearing: a Q112.16 field over ±2^57 whole units (75
bits: two full 32-bit groups, then an 11-bit remainder on top), and a Q64.64
field over the full int64 unit range (128 bits, four groups). Then the end.

The run is pinned as a vector, and so is the whole message. A reader must
decode both exactly and a writer must emit both exactly. [vectors: message-worked-example-string-align-fixed, message-golden-whole]

### The whole message

The message's twenty-nine operations, in the step spelling the shared corpus
uses, with the value each carries:

| # | operation | value |
|---|---|---|
| 1 | `bits 4` | 13 |
| 2 | `bits 11` | 1445 |
| 3 | `bits 24` | 11259375 |
| 4 | `bits 32` | 3735928559 (`0xDEADBEEF`) |
| 5 | `int -100 100` | -37 |
| 6 | `int -2147483648 2147483647` | -123456789 |
| 7 | `bool` | true |
| 8 | `float` | 3.1415926 (`0x40490FDA`) |
| 9 | `compressed_float 0 10 0.01` | 5.0 (`0x40A00000`) |
| 10 | `double` | 1/3 (`0x3FD5555555555555`) |
| 11 | `bits 8` | 127 |
| 12 | `bits 16` | 4660 |
| 13 | `bits 32` | 305419896 |
| 14 | `bits 64` | 1311768467463790320 |
| 15 | `int_relative 100` | 101 |
| 16 | `int_relative 100` | 2100 |
| 17 | `align` | - |
| 18 | `bytes 7` | `DE AD BE EF CA FE 01` |
| 19 | `string 16` | `golden` |
| 20 | `string 8` | `E7 A2 81 E7 9F B3` (碁石) |
| 21 | `align` | - |
| 22 | `fixed 8 8 -100 100` | -832 |
| 23 | `fixed 16 16 -2000 2000` | 80904192 |
| 24 | `fixed 48 16 -100000 100000` | -3559993401 |
| 25 | `fixed 16 16 0 30000` | 1966079999 |
| 26 | `align` | - |
| 27 | `fixed 112 16 -144115188075855872 144115188075855872` | -6472691358699745 |
| 28 | `fixed 64 64 -9223372036854775808 9223372036854775807` | 1512366075204170930115394234220888865 |
| 29 | `end` | - |

A `-` marks an operation with no value of its own. A value written as a
hexadecimal pattern is compared as a bit pattern rather than as a number. The
`fixed` values are raw scaled integers, not real units.

The whole message is 106 bytes:

    0x5D 0xDA 0xF7 0xE6 0xD5 0x77 0xDF 0x56 0xEF 0x9F 0x75 0x19
    0x52 0xBC 0xDA 0x0F 0x49 0x40 0xF4 0x55 0x55 0x55 0x55 0x55
    0x55 0x55 0xFF 0xFC 0xD1 0x48 0xE0 0x59 0xD1 0x48 0xC0 0x7B
    0xF3 0x6A 0xE2 0x59 0xD1 0x48 0x84 0xB7 0x06 0xDE 0xAD 0xBE
    0xEF 0xCA 0xFE 0x01 0x06 0x67 0x6F 0x6C 0x64 0x65 0x6E 0x06
    0xE7 0xA2 0x81 0xE7 0x9F 0xB3 0xC0 0x60 0x00 0x80 0xA2 0x7C
    0xFC 0xEC 0x26 0xCB 0xFF 0xFF 0x4B 0x1D 0x1F 0xEF 0xD2 0x1A
    0x1F 0x01 0xE9 0xFF 0xFF 0x09 0x19 0x2A 0x3B 0x4C 0x5D 0x6E
    0x7F 0x78 0x6F 0x5E 0x4D 0x3C 0x2B 0x1A 0x09 0x04

The last field ends at bit 843, so the final byte carries three bits of message
and five bits of zero padding, and a reader's end consumes them: 848 bits. A
conforming measure reports at least 847 bits, four more than the stream
occupies from an aligned start, because the aligns, the byte block and the
strings each cost more from some other starting position.

These bytes are not copied out of an implementation. They come from encoding
the fields above with a writer written from this document alone, in
`tools/conformance`, and they are printed here because that writer, this byte
block and the C++ implementation's pinned emission agree byte for byte, which
the same tool checks on every run.

## The Measure Stream

**A measure is a bound, not the packet size.** A measure must report a size
sufficient to serialize the message at any starting bit position: alignment
cost depends on the bit position the message is later written at, which a
measure does not know, so no single number is exact for every start. The
expected implementation charges 7 bits per alignment-performing operation
(`align`, `bytes`, `string`) and exact width for everything else, and a tighter
correct bound conforms too. [vectors: sequence-measure-worked-example, sequence-align-pads-after-an-odd-width, sequence-bytes-realigns-a-mid-byte-stream, message-worked-example-string-align-fixed, message-golden-whole, end-accept-after-three-bits, end-accept-after-bytes]

The worked example: `{ bits(8); align; bits(8) }` is 16 bits written from an
aligned start, where the align is a no-op, and spans 23 bits written from bit
offset 1. An exact-from-zero measure reports 16 and is non-conforming; the
conservative 8 + 7 + 8 = 23 is sufficient everywhere. A measure refuses nothing,
and every implementation ships one. [vectors: sequence-measure-worked-example]

## Reader Obligations

The operations above state what the bytes mean. This section states what a
reader does with bytes it cannot trust.

**Reading past the end must be refused**, as `past_end`. An operation that
would consume more bits than remain in the stream fails; it does not produce a
partial value, zero-fill the missing bits, or wrap. A stream's length is a count
of bytes, so the test is `bit_index + n > 8 * bytes`. [vectors: bits-refuse-past-end-empty-stream, bits-refuse-past-end-16-of-8, bits-accept-16-of-16, bits-refuse-past-end-33-of-32, bits-refuse-past-end-64-of-56, bool-refuse-past-end-empty-stream, uint128-refuse-past-end-15-bytes, uint128-refuse-past-end-empty-stream, int-refuse-past-end, int-accept-ten-bits-of-ten, int-refuse-past-end-empty-stream, int64-refuse-past-end-41-of-32, int64-accept-41-bits-of-41, int64-refuse-past-end-empty-stream, int128-refuse-past-end, int128-refuse-past-end-empty-stream, fixed-refuse-past-end, fixed-refuse-past-end-empty-stream, ladder-refuse-past-end-empty-stream, bounded-9-refuse-past-end-payload-truncated, bounded-13-refuse-past-end-payload-truncated, bounded-17-refuse-past-end-payload-truncated, absolute-refuse-past-end-payload-truncated, float-refuse-past-end-24-of-32, float-refuse-past-end-empty-stream, double-refuse-past-end-56-of-64, double-refuse-past-end-empty-stream, compressed-float-refuse-past-end-8-of-10, compressed-float-refuse-past-end-empty-stream, bytes-refuse-past-end, bytes-refuse-past-end-after-the-align, string-refuse-past-end-payload-truncated, string-refuse-past-end-empty-stream, object-refusal-past-the-end-propagates]

**Refusal is the only answer to a stream this document refuses.** A trap, a
crash or an abort on malformed input is non-conforming, so a runtime whose
integer arithmetic traps on overflow uses wrapping operations on the read path.
Every refused vector in the corpus is a stream a reader must refuse and survive.
[vectors: int-negative-min-refuse-offset-all-ones, int128-128-bit-span-refuse-offset-one-past-the-span, fixed-q64-64-refuse-offset-all-ones, absolute-refuse-all-bits-set, compressed-float-values-clamped-refuse-integer-all-thirty-two-bits]

**A refused read must leave its destination unwritten.** When a read of a
scalar fails, the caller's value is exactly what it was before the call. A read
into a caller-owned buffer, which is `bytes` and `string`, leaves that buffer's
contents unspecified after a refusal, and a composite read (`object`, or any
sequence of reads) may leave earlier members written, because each primitive
read carries the rule alone. [vectors: int-range-0-8-refuse-offset-one-past-the-span, float-refuse-quiet-nan, double-refuse-negative-infinity, compressed-float-refuse-decode-rounding-to-infinity, absolute-refuse-non-canonical-difference-of-one, bool-refuse-past-end-empty-stream]

**Failure is terminal.** Nothing after a failing operation has a defined
position, so nothing after it is interpretable. After a refusal, every later
read on that stream must be refused too, consuming no bits and writing no
destination, zero-bit reads included: a degenerate ranged field, an align on an
aligned stream, a `bytes` of zero count, an `object`, and an `end`. A failure
persists until the stream is re-initialized onto a new buffer, or discarded. A
stream that latches a failure state conforms, and so does one whose failing read
returns no successor stream. [vectors: sequence-refusal-is-terminal-after-bad-alignment, sequence-refusal-is-terminal-after-a-range-refusal, sequence-refusal-is-terminal-before-a-zero-bit-field, sequence-refusal-is-terminal-before-a-zero-bit-fixed-field, sequence-refusal-is-terminal-after-a-string-refusal, end-refuse-after-a-refused-field-keeps-the-first-reason] [controls: fixed-degenerate-terminal]

**Every value has exactly one encoding.** The refusal rules above, taken
together, mean a reader refuses every stream a conforming writer cannot produce:
padding that is not zero, an integer outside its declared range, a tier a writer
would not choose, a float a writer is never handed, text that is not text, and
anything after the end. So one value is always the same bytes, and two
implementations that agree on the values agree on the bytes. Each of those
rules is stated, and cited, in its own section. [vectors: align-refuse-one-non-zero-pad-bit, int-range-0-8-refuse-offset-one-past-the-span, absolute-refuse-non-canonical-difference-of-one, float-refuse-quiet-nan, string-refuse-overlong-two-byte-slash, end-refuse-one-zero-byte-after-the-empty-object, end-refuse-lowest-padding-bit]

## Refusal reasons

A refusal names the rule that refused it, from this closed list. Every
implementation spells these names identically wherever it exposes them, and
every refused vector states the reason it expects. A reader must refuse for the
reason the vector states, and a refusal for any other reason is a disagreement
about the format however red the stream ends up. The reason reported is the
**first** refusal's: the later refusals of a failed stream are its echoes.
[vectors: string-refuse-nul-as-the-only-byte, string-refuse-nul-in-the-middle, end-refuse-after-a-refused-field-keeps-the-first-reason, sequence-refusal-is-terminal-after-a-range-refusal] [controls: wrong-reason]

| reason | the rule that refuses |
|---|---|
| `past_end` | a read needs more bits than the stream holds |
| `value_out_of_range` | a decoded value outside its declared range or domain: a ranged offset past its span, a string length past its buffer, a compressed integer past `max_integer_value`, an `int_relative` outside its domain or not increasing |
| `nonzero_padding` | a set bit in an align's padding, or in the unused bits of the final byte at the end |
| `trailing_data` | a whole byte or more after the end of the object |
| `non_finite_float` | a `float` or `double` that is NaN or an infinity, or a `compressed_float` decode that is not finite |
| `invalid_utf8` | a string payload that is not well-formed UTF-8 |
| `embedded_nul` | a zero byte among a string's transmitted bytes |
| `non_canonical` | an encoding a conforming writer never produces for the value it carries: the `int_relative` absolute tier carrying a bounded difference |

## Compatibility Notes

* **The format is not self-describing.** There are no tags, lengths or type
  markers beyond what the operations imply. A stream is meaningless without the
  exact sequence of calls that produced it.
* **Ranges are part of the format.** Changing a `min` or `max` on one side
  changes the bit width and desynchronizes everything after it.
* **Alignment is part of the format.** `bytes` and `string` align; `bits`,
  `int`, `fixed` and `uint128` do not.
* **Zero-bit fields are legal.** `min == max` writes nothing at all.
* **A packet is one object and its end.** A reader handed more bytes than the
  object occupies refuses at the end, so the buffer handed to a reader is the
  packet exactly. [vectors: end-refuse-zero-byte-after-an-aligned-object, message-golden-whole]

## The conformance corpus

**The shared corpus is the conformance instrument.** It is the `conformance/`
directory of this repository, one file per covered operation plus the sequence
and message files. An implementation conforms when it reproduces every vector
byte for byte and refuses every refused vector for its stated reason. [vectors: message-golden-whole, end-refuse-after-a-refused-field-keeps-the-first-reason] [controls: wrong-reason]

| file | what it pins |
|---|---|
| `bits.txt` | every width from 1 to 64, the group order above 32, and the 64-bit edges a double loses |
| `bool.txt` | one bit, and a read that looks at no other bit |
| `uint128.txt` | 128 raw bits, the low half first, and the edges around `2^64` |
| `align.txt` | zero bits to the boundary, nothing when aligned, set padding refused |
| `int.txt` | the width rule, the degenerate range, the offset refusal |
| `int64.txt` | the same at 64 bits, the group boundary, and the 64-bit edges a double loses |
| `int128.txt` | the ranged 128-bit codec, ranges wider than `2^127`, and the 128-bit edges |
| `fixed.txt` | the offset over the raw bounds at six Q formats, and the degenerate range on every width |
| `int_relative.txt` | every tier, the domain refusal in every tier, each payload's span, and the canonical tier |
| `float.txt` | finite patterns kept bit exact, non-finite patterns refused |
| `double.txt` | the same at 64 bits, and the group order |
| `compressed_float.txt` | both clamps, the two roundings on write and read, writer inputs between quanta and in the clamp band, and the finite decode |
| `bytes.txt` | the align first, and a zero count that still performs it |
| `string.txt` | the length rule, UTF-8 refusals, the NUL refusal, and real text in every script round tripped byte for byte |
| `end.txt` | trailing data and padding refused, an exact stream accepted |
| `object.txt` | no bytes of its own, each vector twinned with the same operations unnested |
| `sequence.txt` | alignment after an odd width, zero-bit fields, terminal failure, and the measure floor |
| `message.txt` | the worked example's run, and the whole golden message |

**A runner discovers the directory and runs every vector in it.** A runner must
enumerate the directory, at run time or at build time from a glob that re-runs
when the directory changes, rather than name the files. An empty directory fails
the run, and so does a vector whose operation the runner has no code for: an
operation nobody implemented is red, never skipped. [controls: unknown-operation]

**The vector format.** A vector file is text. `#` begins a comment at the start
of a line and nowhere else, blank lines separate records, and each record is
`key` and value, one per line:

| key | meaning |
|---|---|
| `operation` | the operation under test, once per record |
| `name` | a stable identifier for the vector, unique across the corpus |
| `param` | one parameter as `name = value`, repeated once per parameter |
| `bytes` | the stream, as hexadecimal byte pairs, empty for a zero-bit read |
| `expect` | `refused` and the reason, or `value = ` and the decoded value, or `bits = 0x` and the decoded bit pattern |
| `consumed` | bits a conforming reader consumes, accepted reads only |
| `writer` | the word `canonical`, on a vector that also pins the bytes a writer emits for the decoded value |
| `writer_input` | `bits = 0x` and a float32 pattern, on a `compressed_float` vector that pins the bytes a writer emits when handed that input |
| `measure_at_least` | the smallest number of bits a conforming measure may report, on a sequence |

Numbers are written as signed decimal or as `0x` hexadecimal, and a parser
accepts values up to 128 bits wide. A `param` takes the type its operation's
section gives it, so `res` under `compressed_float` is a `float32`. A `string`
value is the transmitted bytes as hexadecimal pairs.

**A sequence vector states more than one operation.** Its `operation` is
`sequence`, its steps are `param step = ` lines in order, and `expect value`
lists one entry per step separated by ` | `, with `-` for a step with no value
of its own. There is one step spelling per operation, listed at the head of
`conformance/sequence.txt`, so any message the format can carry can be stated
as a sequence, the golden message included. `object <n>` wraps the next `n`
steps in a nested object.

**A harness presents every stream with the slack its implementation's buffer
contract requires**, filled with a non-zero pattern, so a decode that depends on
memory past the end cannot pass by reading zeros. The bytes of a vector are the
stream, and the slack is not part of it.

**What a runner checks.** A runner that checks less than this is not running
the instrument, and must check each of the following. [vectors: message-golden-whole, compressed-float-writer-input-fusion-witness-2p23, sequence-measure-worked-example] [controls: unknown-operation, wrong-reason]

* for an accepted vector, require the decoded value, compared as a number or as
  a bit pattern where the record states `expect bits`, and the bits consumed;
* for a vector carrying `writer canonical`, require the bytes a writer emits for
  the decoded value, byte for byte;
* for a vector carrying `writer_input`, require the bytes a writer emits for
  that input, byte for byte;
* for a refused vector, require the refusal, its stated reason, the scalar
  destination unwritten, and the stream terminal: a further read on the same
  stream also fails, consumes no bits and writes nothing; [vectors: end-refuse-after-a-refused-field-keeps-the-first-reason, sequence-refusal-is-terminal-after-a-range-refusal] [controls: wrong-reason]
* for a sequence carrying `measure_at_least`, require a measure of at least
  that many bits.

**A gate nobody has seen go red is a claim, not a measurement.** Two kinds of
control prove the instrument responds, and both run in the test chain: a corpus
file whose operation has no runner, which every gate must reject rather than
skip; and, for each refusal rule, a reader with that one rule deleted, run
against the real corpus, which must fail exactly the vectors its
`conformance-controls/<rule>.expected` file names and no others. A control that
stops finding anything to break is a failure of the control and not a licence
to remove it. [controls: unknown-operation, align-padding, ranged-reconstruction, ranged-range, fixed-degenerate-terminal, float-finite, double-finite, compressed-float-finite, end-trailing-data, end-padding, int-relative-canonical, string-embedded-nul, string-utf8, string-utf8-surrogate, int64-through-double, wrong-reason]

**Vectors discriminate.** A value from the middle of a range, or one where every
plausible reading agrees, proves nothing: the `compressed_float` divergence
survived years of green suites because every pinned value landed on a quantum.
Each rule's vectors include the values a wrong reading gets wrong.

## Provenance

Written 2026-07-21 by Rowan, by reading the then-existing implementation and
verifying every claim against its golden test vector, and revised for format
2.0 on 2026-10-22.

**This document is the authority.** Where this document and any implementation
disagree, the implementation is a bug, and where an implementation lags a rule,
its repository names the gap and the release that closes it. `serialize.h` is
one implementation among the family, and was the first, which is a fact about
history and not about standing. Until 2026-08-14 this section made the C++
implementation the tie-break wherever the document was silent; 2.0 leaves no
silence for it to break, because every rule here cites the vector that proves
it, and what no vector can prove is no longer stated.

The corpus is the conformance instrument and is never generated from an
implementation: a suite that regenerates its own expectations proves only that a
port agrees with itself, which is how one wrong reading travels to every
implementation under green results.
