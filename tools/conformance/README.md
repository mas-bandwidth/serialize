# Conformance: STANDARD.md vs the implementation

`STANDARD.md` specifies serialize's wire format. This decodes the library's own
golden vector using **only what that document says** — the LSB-first bit
packing, the ranged-integer widths, the alignment rules, the relative-integer
ladder — and asserts every field.

    python3 tools/conformance/verify_standard.py

No compiler needed: `golden_wire_bytes` and the expected values are read out of
`serialize.h` as data. Exit 0 means the document and the code agree.

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
