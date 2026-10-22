# Conformance: STANDARD.md vs the implementation

`STANDARD.md` specifies serialize's wire format. This decodes the library's own
golden vectors using **only what that document says** — the LSB-first bit
packing, the ranged-integer widths, the alignment rules, the relative-integer
ladder, the two-rounding `compressed_float` arithmetic — and asserts every
field. It also checks the other half of conformance, which the document states
in as many words: an implementation conforms when it reproduces every vector
byte for byte **and refuses everything the document says must be refused**.

    cd tools/conformance && go run .

This is one of the two conformance instruments in this repository, and they ask
different questions. This tool asks whether STANDARD.md, read on its own,
describes the bytes the implementations actually emit — it decodes pinned real
emissions with a decoder written from the document. The shared corpus in
`conformance/` asks the other half: whether this implementation's reader accepts
and refuses exactly what the document says every implementation in the family
must, vector for vector. [conformance.cpp](../../conformance.cpp) runs the corpus
as the ctest target `conformance`. Neither replaces the other.

## The whole golden message

STANDARD.md prints the message's 106 bytes and `conformance/message.txt` carries
them as a vector. Neither is copied out of `serialize.h`. The steps and values are
stated in [golden_message.go](golden_message.go), encoded by the corpus encoder in
[corpus.go](corpus.go), which is written from the document, and three things must
then agree byte for byte: that encoding, the block the page prints, and the
library's pinned `golden_wire_bytes`. Any one of them drifting turns this tool red.

Set `SERIALIZE_PRINT_GOLDEN=1` to print the encoding, its bit count and its measure
floor, which is how the page's block and the vector are written when the message
changes.

Standard-library Go only, no compiler for the C++ needed: `golden_wire_bytes`,
`golden_uint128_bytes`, `golden_int128_bytes`, `pinned_bytes` and the expected
values are read out of `serialize.h` as data. Exit 0 means the document and the
code agree.

## Why the golden vector is the right oracle

It is a hand-verified byte-exact snapshot of a message exercising every
primitive: all four raw bit widths, a narrow and a full-range ranged int, a
bool, a float, a quantized compressed float, a double, four unsigned widths,
both interesting tiers of the relative-integer ladder, a byte block, a narrow
string, a second string of real multibyte text (碁石, go stones, as its UTF-8
bytes), and six fixed point fields across signed, unsigned, 16/32/64/128-bit
storage, then the end of the object. If a decoder written from the document
reproduces all of them, consumes exactly the right number of bits and finds
the stream ending where the message does, the document is right.
The library's second, additive pin — `golden_uint128_bytes` — is decoded the
same way to verify the uint128 half order.

The final check — that decoding consumed exactly the golden byte count — is the
one that catches alignment errors. Fields can decode correctly while the bit
cursor drifts.

## Why the golden vector is not enough

The golden's compressed float is 5.0 over [0,10]: it normalizes to exactly 0.5
and lands exactly on a quantum, where `float32`, `double` and a fused
multiply-add all produce the same integer. The arm64 divergence shipped with
every suite green precisely because every pinned value in the family had that
property. So the checker additionally carries the discriminating battery:

- **Writer quantization vectors** that land *between* quanta — where one
  rounding (an FMA) and two roundings disagree about the written integer, and
  where widening to `double` writes a different integer again.
- **Reader decode vectors pinned to exact float32 bit patterns** over a
  non-zero `min`, because a fused reader decode is one ulp off and no
  tolerance comparison can see one ulp. The stream for the main trio is the
  library's own additive pin (`pinned_bytes`).
- **Refusal vectors**: streams a conforming reader must reject — a ranged int
  or int128 or fixed offset past its span, non-zero alignment padding, a
  non-increasing `int_relative` absolute form, a `compressed_float` integer
  above `max_integer_value` — decoded by the document's rules, where a decode
  that *succeeds* is the failure. Each accept-boundary twin pins the largest
  legal value, so the refusal is exactly one past it.
- **Degenerate-range vectors** decoded from an empty stream, proving
  `min == max` costs zero bits on every storage width.

## Refusal reasons

Every refusal this checker's decoder makes names one reason from STANDARD.md's
closed list — `past_end`, `value_out_of_range`, `nonzero_padding`,
`trailing_data`, `non_finite_float`, `invalid_utf8`, `embedded_nul`,
`non_canonical` — and every refused vector states the one it expects as
`expect refused <reason>`. A vector refused for a different reason FAILS, a
sequence reports its first refusal, and a refused vector with no reason, or a
reason outside the list, is a malformed file.

Format version 2.0 refusals the checker carries from the document alone:
`float` and `double` patterns that are NaN or an infinity (`non_finite_float`;
`-0` and subnormals pass bit exact), a `compressed_float` decode that rounds
to an infinity (witness `[3 * 2^103, FLT_MAX]` at resolution `2^104`), an
`int_relative` absolute tier carrying a difference a bounded tier carries
(`non_canonical`), and the `end` of an object with a whole byte after it
(`trailing_data`) or non-zero unused bits in its final byte
(`nonzero_padding`). `wstring` is gone from the format, so a `wstring` vector
is an operation this checker cannot drive, and fails.

A `compressed_float` record may carry `writer_input bits = 0x...`: the float32
a writer is handed, which must quantize to exactly the record's bytes. That is
how the corpus pins between-quanta writer inputs, which a decode alone cannot.

## The end of the object and the writer obligation

STANDARD.md's `end` makes the writer's zeroed trailing bits checkable on the
read path: no field read looks past its own bits, and `end` consumes the final
byte's unused bits, refusing them non-zero, and refuses any whole byte after
the object. The checker holds pinned real emissions to the writer obligation —
`golden_wire_bytes` (the C++ writer) and the `int_relative` tier-boundary
streams (serialize.c) — and proves on a doctored stream that the field still
decodes while `end` refuses it.
