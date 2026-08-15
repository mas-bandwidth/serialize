# serialize 1.0

This document specifies the **wire format** produced and consumed by the
serialize library, precisely enough to write an independent implementation that
interoperates byte-for-byte.

It describes a format, not an implementation. Nothing here constrains how you
structure your code.

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
release path should carry — assert in debug only, everywhere. The read side is
untouched by the doctrine: readers face untrusted data, and every refusal rule
this document states binds in every build mode.

## Bit-Level Primitives

### bits

    serialize_bits( stream, value, bits )

Writes the low `bits` bits of `value`, where `bits` is in `[1,64]`.

* For `bits <= 32` this is a single group of that many bits, and the value must
  be less than `2^bits`.
* For `bits > 32` the value is split: the **low 32 bits are written first as a
  32-bit group**, then the remaining `bits - 32` high bits as a second group.

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

`value - min` is written in that many bits. Note the consequences:

* a range of `[0,7]` costs 3 bits;
* a range of `[0,8]` costs 4 bits;
* a degenerate range where `min == max` costs **zero bits** — the value is
  known from the range alone and nothing is written.

Readers must check that the decoded value lies within `[min,max]` and fail
otherwise.

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

### fixed (Q format, ranged)

    serialize_fixed( stream, value, integer_bits, fraction_bits, min, max )

A fixed point value held in an integer storage type of exactly `integer_bits +
fraction_bits` bits, with the sign bit counting toward `integer_bits` (Q48.16
in an `int64_t`, Q112.16 in an `__int128`). The stored integer is the real
value scaled by `2^fraction_bits`. `min` and `max` are bounds in **whole real
units**, and all four parameters are compile-time constants of the call site —
they are part of the format, exactly like a ranged integer's bounds.

The encoding is an offset encoding over the **raw** (scaled) bounds:

    raw_min = min << fraction_bits
    raw_max = max << fraction_bits
    bits    = bit length of ( raw_max - raw_min )    — bits_required, at whatever width the range needs

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
pin a negative tie value that distinguishes the two rules.

### int_relative

    serialize_int_relative( stream, previous, current )

Encodes an increasing sequence compactly, where `current > previous`. Let
`difference = current - previous`. The encoding is a ladder of one-bit flags,
each answering "does it fit in this tier?":

| flag sequence | payload | difference range |
|---|---|---|
| `1` | — | exactly 1 |
| `0 1` | `serialize_int( d, 2, 6 )` — 3 bits | 2 – 6 |
| `0 0 1` | `serialize_int( d, 7, 23 )` — 5 bits | 7 – 23 |
| `0 0 0 1` | `serialize_int( d, 24, 280 )` — 9 bits | 24 – 280 |
| `0 0 0 0 1` | `serialize_int( d, 281, 4377 )` — 13 bits | 281 – 4377 |
| `0 0 0 0 0 1` | `serialize_int( d, 4378, 69914 )` — 17 bits | 4378 – 69914 |
| `0 0 0 0 0 0` | `current` as 32 raw bits | anything |

A difference of 1 — the common case for sequence numbers — costs a single bit.

**The final tier transmits `current`, not the difference.** Every tier above it
encodes the difference, so this reads as an inconsistency and is not: at full
width the subtraction buys nothing, and sending the absolute value lets the
reader check `current > previous` directly. A reader must perform that check
and fail if it does not hold, since the absolute form carries no ordering
guarantee of its own.

**The semantics are pinned: no wrapping** *(adopted 2026-08-15 from the schema
enactment; the ruling verbatim: "no wrapping sequence numbers. meant for
positive only and up to maximum only.")*. `serialize_int_relative` is strictly
increasing — `current > previous`, the reader fails otherwise — and no wrap
semantics exist: a caller with a wrapping counter unwraps it before
serializing. Wrap-around is not an encoding this operation carries, not now
and not by future amendment.

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
`max_integer_value`, adds `0.5`, takes the floor, and writes the result in
`bits` bits. The reader divides by `max_integer_value`, multiplies by `delta`,
and adds `min`.

**This arithmetic is `float32`, and the two roundings are part of the format.**
The product `normalized * max_integer_value` rounds to `float32` BEFORE `0.5`
is added, and that sum rounds to `float32` before the floor. Two roundings, not
one. Specifically, an implementation must not:

- widen any step to `double` (or any wider type) before the floor, and
- contract the multiply and the add into a fused multiply-add, which rounds
  once instead of twice. Languages that permit contraction must suppress it
  here — in C and C++ by storing the product through a `float` local (or
  `-ffp-contract=off`), in Go by an explicit `float32()` conversion around the
  product. Rust does not fuse unless `mul_add` is called explicitly.

This is not pedantry; it changes the bytes. Over `[0, 10]` at resolution
`0.01`, the required arithmetic quantizes `0.005` to `1`, `0.025` to `3`,
`0.105` to `11` and `9.995` to `1000`; widening to `double` yields `0`, `2`,
`10` and `999`. A value landing exactly on a quantum — `2.5` here — agrees
under every variant, so a conformance vector built only from such values will
pass while the wire is wrong. Vectors must include values that land between
quanta.

Readers must reject an integer greater than `max_integer_value`.

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

**`string` payloads are well-formed UTF-8 by contract** *(adopted 2026-08-15
from the schema enactment, writer-trusted per the doctrine above)*. The wire
shape is unchanged; what the `string` spelling adds is a **contract**: the
payload is well-formed UTF-8, the writer's obligation. Writing malformed UTF-8
is a writer contract violation — debug-only asserts where the language
supports them — and the conformance vectors carry only valid UTF-8. An
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
  handing it to the application.
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
bytes**.

1. The length, as `serialize_int( length, 0, buffer_size - 1 )`.
2. Each character as a **32-bit group**, in order.

**No alignment is performed anywhere in this operation** — this is the one
place where the wide-string path deliberately differs from its narrow
counterpart, which aligns via `serialize_bytes`. An implementation that mirrors
the narrow string path here will produce the wrong bytes.

Wide characters are transmitted as 32 bits regardless of the local `wchar_t`
width. A reader whose `wchar_t` cannot hold a received value **fails the read
rather than truncating**.

**Each 32-bit group carries one UTF-16 code unit — not one code point — and
the payload is well-formed UTF-16 by contract** *(adopted 2026-08-15 from the
schema enactment, writer-trusted per the doctrine above)*. Surrogate **pairs**
are valid — full Unicode, an astral character is two groups; an **unpaired**
surrogate is a writer contract violation, debug-asserted where the language
supports it. 2-byte and 4-byte `wchar_t` platforms must produce **identical
bytes**: the 4-byte platform converts at the boundary — splits astral code
points into surrogate pairs on write, recombines on read — because the
platform-compatibility claim this section used to make was false for astral
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
or out-of-range values are the caller's contract violation, asserted in debug
builds where the language supports it, and a measure never refuses at runtime
in release. A measure sits on the trusted side of the boundary — nothing it
sees came off a network.

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
specified here because five implementations ship it today and had already
begun to disagree, not because it is expected to survive into entropy-coded
encodings.

**Testable**: for every message in the conformance corpus,
`measure >= bits written`, at every starting bit position; and the worked
example discriminates — a conservative measure reports 23 bits for
`{ bits(8); align; bits(8) }` where an exact-from-zero measure reports 16.

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

**This document is the format. Where this document and any implementation
disagree, the implementation is a bug.** There is no reference implementation:
`serialize.h` is one implementation among five — C, C++, C#, Go and Rust — and
holds no special authority. It was the first, which is a fact about history and
not about standing.

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

**Conformance vectors must discriminate.** A value taken from the middle of a
range, or one that lands where every plausible reading agrees, proves nothing —
the `compressed_float` divergence above survived years of green test suites
because every pinned value in every implementation landed exactly on a quantum,
where `float32`, `double` and a fused multiply-add all produce the same answer.
A vector that cannot fail is not evidence.
