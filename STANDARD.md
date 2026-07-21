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
stream. The measure stream computes the size a message would occupy without
producing bytes. This document concerns only the bytes on the wire.

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

## Bit-Level Primitives

### bits

    serialize_bits( stream, value, bits )

Writes the low `bits` bits of an unsigned 32-bit `value`, where `bits` is in
`[1,32]`. The value must be less than `2^bits`.

### bool

    serialize_bool( stream, value )

One bit: `1` for true, `0` for false.

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

### int64

    serialize_uint64 / serialize_int64

As above but with a 64-bit range. If `bits_required64( min, max )` is 32 or
fewer, the value is written as a single group of that many bits. Otherwise the
low 32 bits are written first, followed by the remaining `bits - 32` high bits.

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
| `0 0 0 0 0` | 32 raw bits | anything |

A difference of 1 — the common case for sequence numbers — costs a single bit.

## Floating Point

### float

    serialize_float( stream, value )

The 32 bits of the IEEE-754 single-precision representation, written as a
32-bit group. No conversion, no compression.

### double

    serialize_double( stream, value )

The 64 bits of the IEEE-754 double-precision representation, written as one
64-bit group.

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

Readers must reject an integer greater than `max_integer_value`.

This is lossy by construction: a round trip returns the nearest representable
quantum, not the original value.

## Bytes and Strings

### bytes

    serialize_bytes( stream, data, count )

**Aligns first**, then writes `count` raw bytes. The alignment is part of the
format, not an optimization — a reader that does not align will desynchronize.

`count` is not written. Both sides must already agree on it.

### string

    serialize_string( stream, string, buffer_size )

A null-terminated narrow string.

1. The length, as `serialize_int( length, 0, buffer_size - 1 )`. The bit cost
   therefore depends on `buffer_size`, which both sides must agree on.
2. The characters, as `serialize_bytes` — **which aligns**.

The terminator is not transmitted; the reader appends it.

Because `buffer_size` is an operand rather than a transmitted value, the same
string serialized against different buffer sizes produces different bytes.

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
width, so streams are compatible between platforms with 2-byte and 4-byte
`wchar_t`. Code points are not translated between UTF-16 and UTF-32: a reader
whose `wchar_t` cannot hold a received value **fails the read rather than
truncating**.

## Worked Example

The library's golden test serializes a fixed message and asserts an exact
72-byte output. The final field is a wide string in a `wchar_t[8]` buffer
containing three characters — `0x043C`, `0x0438`, `0x0440` — and it produces
this 13-byte tail:

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

Total: 3 + 3×32 = 99 bits = 13 bytes after flush. This matches, and it is the
cheapest way to confirm an independent implementation is correct.

## Compatibility Notes

* **The format is not self-describing.** There are no tags, lengths, or type
  markers beyond what the operations imply. A stream is meaningless without the
  exact sequence of calls that produced it. This is the source of its
  compactness and the reason both endpoints must ship compatible code.
* **Ranges are part of the format.** Changing a `min`/`max` on one side changes
  the bit width and silently desynchronizes everything after it. Range changes
  are breaking changes.
* **Alignment is part of the format.** `serialize_bytes` and `serialize_string`
  align; `serialize_bits`, `serialize_int` and `serialize_wstring` do not.
* **Zero-bit fields are legal.** `min == max` writes nothing at all.

## Provenance

Written 2026-07-21 by Rowan, by reading the reference implementation and
verifying every claim against the library's golden test vector. It documents
the format as it stands; where this document and the implementation disagree,
the implementation is authoritative and this document is a bug.

The reference implementation is `serialize.h`. The verifying test vector is
`golden_wire_bytes` in that file.
