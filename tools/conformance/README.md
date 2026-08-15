# Conformance: STANDARD.md vs the implementation

`STANDARD.md` specifies serialize's wire format. This decodes the library's own
golden vectors using **only what that document says** — the LSB-first bit
packing, the ranged-integer widths, the alignment rules, the relative-integer
ladder, the two-rounding `compressed_float` arithmetic — and asserts every
field. It also checks the other half of conformance, which the document states
in as many words: an implementation conforms when it reproduces every vector
byte for byte **and refuses everything the document says must be refused**.

    cd tools/conformance && go run .

Standard-library Go only, no compiler for the C++ needed: `golden_wire_bytes`,
`golden_uint128_bytes`, `golden_int128_bytes`, `pinned_bytes` and the expected
values are read out of `serialize.h` as data. Exit 0 means the document and the
code agree.

## Why the golden vector is the right oracle

It is a hand-verified byte-exact snapshot of a message exercising every
primitive: all four raw bit widths, a narrow and a full-range ranged int, a
bool, a float, a quantized compressed float, a double, four unsigned widths,
both interesting tiers of the relative-integer ladder, a byte block, a narrow
string, a wide string, and four fixed point fields across signed, unsigned,
16/32/64-bit storage. If a decoder written from the document reproduces all
of them and consumes exactly the right number of bits, the document is right.
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

Two refusal classes are deliberately absent because each turns on a ruling
PR #60 leaves open: trailing bits after the final operation, and reads past
the end of the caller's buffer. Vectors for those land when the rulings do.
