#!/usr/bin/env python3
"""Check serialize/STANDARD.md against the implementation.

Decodes the library's own golden wire-format vectors using ONLY what
STANDARD.md states — the bit packing, the ranged-integer widths, the alignment
rules, the relative-integer ladder, the fixed-point offset encoding, the
uint128 half order — and asserts every field of the golden message plus the
additive uint128 pin. Nothing here consults serialize.h's implementation; the
golden arrays and the expected values are read out of the header as data.

usage: python3 tools/conformance/verify_standard.py
exit:  0 = the document matches the implementation, 1 = it does not
"""
import math, os, re, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HDR = os.path.join(ROOT, "serialize.h")


class BitReader:
    """STANDARD.md, 'General Conventions': LSB-first into 64-bit little-endian words."""
    def __init__(self, b): self.b = b; self.i = 0
    def bits(self, n):
        v = 0
        for k in range(n):
            v |= ((self.b[self.i // 8] >> (self.i % 8)) & 1) << k
            self.i += 1
        return v
    def align(self):
        pad = (8 - (self.i % 8)) % 8
        if pad and self.bits(pad) != 0:
            raise ValueError("alignment padding must be zero")
    def read_bytes(self, n):
        self.align()
        out = self.b[self.i // 8: self.i // 8 + n]; self.i += n * 8
        return out


def bits_required(lo, hi):
    return 0 if lo == hi else (hi - lo).bit_length()

def sint(r, lo, hi):
    n = bits_required(lo, hi)
    return (r.bits(n) if n else 0) + lo

def relative(r, prev):
    """STANDARD.md, 'int_relative': the SIX-tier flag ladder, then absolute.

    Two corrections, 2026-08-14, and this function had both halves of the same
    historical bug the prose was fixed for months ago:

      * the [4378, 69914] tier was missing entirely, so every stream using it
        decoded as the absolute form and every stream past it shifted;
      * the final tier transmits `current` ABSOLUTELY, not a difference, so
        `prev + r.bits(32)` was wrong by exactly `prev`.

    This tool exists to decode the golden stream using ONLY the document, so
    that the document is checked against something other than the library it
    was written from. It reported "29 checks, 0 failures" throughout, while
    being wrong on ten of the twelve tier-boundary streams. A checker that
    cannot fail is not a checker.
    """
    if r.bits(1): return prev + 1
    if r.bits(1): return prev + sint(r, 2, 6)
    if r.bits(1): return prev + sint(r, 7, 23)
    if r.bits(1): return prev + sint(r, 24, 280)
    if r.bits(1): return prev + sint(r, 281, 4377)
    if r.bits(1): return prev + sint(r, 4378, 69914)
    current = r.bits(32)
    # the absolute form carries no ordering guarantee of its own, so the
    # reader must check it -- STANDARD.md says so in as many words
    if current <= prev:
        raise ValueError(
            "int_relative: absolute tier decoded current=%d which is not "
            "greater than previous=%d" % (current, prev))
    return current


def hex_array(name):
    src = open(HDR).read()
    m = re.search(name + r"\[\]\s*=\s*\{(.*?)\};", src, re.S)
    if not m: raise SystemExit(name + " not found in serialize.h")
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))


def fixed(r, fraction_bits, lo, hi):
    """STANDARD.md, 'fixed': offset encoding over the raw (scaled) bounds,
    written in 32-bit groups from least significant upward."""
    raw_lo, raw_hi = lo << fraction_bits, hi << fraction_bits
    n = bits_required(raw_lo, raw_hi)
    v = 0
    for g in range(0, n, 32):
        v |= r.bits(min(32, n - g)) << g
    if v > raw_hi - raw_lo:
        raise ValueError("fixed offset out of range")
    return v + raw_lo


def ranged128(r, lo, hi):
    """STANDARD.md, 'int128 (ranged)': offset encoding computed in the unsigned
    128-bit domain, written in 32-bit groups from least significant upward with
    the final group carrying the remainder."""
    mask = (1 << 128) - 1
    lo &= mask
    hi &= mask
    n = 0 if lo == hi else ((hi - lo) & mask).bit_length()
    v = 0
    for g in range(0, n, 32):
        v |= r.bits(min(32, n - g)) << g
    if v > ((hi - lo) & mask):
        raise ValueError("int128 offset out of range")
    out = (v + lo) & mask
    return out - (1 << 128) if out >= (1 << 127) else out


def main():
    data = hex_array("golden_wire_bytes")
    r = BitReader(data)
    fails, n = [], 0

    def eq(name, got, exp, tol=None):
        nonlocal n; n += 1
        ok = abs(got - exp) < tol if tol else got == exp
        if not ok: fails.append(f"{name}: got {got!r}, expected {exp!r}")

    # field order per GoldenWireSerialize; values per GoldenWireInit
    eq("bits4", r.bits(4), 13)
    eq("bits11", r.bits(11), 1445)
    eq("bits24", r.bits(24), 11259375)
    eq("bits32", r.bits(32), 0xDEADBEEF)
    eq("int_small (ranged -100..100)", sint(r, -100, 100), -37)
    v = sint(r, -2**31, 2**31 - 1)
    eq("int_full (full int32 range)", v - 2**32 if v >= 2**31 else v, -123456789)
    eq("bool flag", r.bits(1), 1)
    eq("float", struct.unpack("<f", struct.pack("<I", r.bits(32)))[0], 3.1415926, tol=1e-6)
    mx = math.ceil(10.0 / 0.01)
    eq("compressed_float", r.bits(bits_required(0, mx)) / mx * 10.0, 5.0, tol=1e-4)
    lo, hi = r.bits(32), r.bits(32)
    eq("double", struct.unpack("<d", struct.pack("<Q", lo | (hi << 32)))[0], 1 / 3, tol=1e-12)
    eq("uint8", sint(r, 0, 255), 0x7F)
    eq("uint16", sint(r, 0, 65535), 0x1234)
    eq("uint32", r.bits(32), 0x12345678)
    lo, hi = r.bits(32), r.bits(32)
    eq("uint64", lo | (hi << 32), 0x123456789ABCDEF0)
    eq("int_relative near (difference 1 = one bit)", relative(r, 100), 101)
    eq("int_relative far (difference 2000)", relative(r, 100), 2100)
    r.align()
    eq("bytes", r.read_bytes(7), bytes([0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0x01]))
    ln = sint(r, 0, 15)
    eq("string", r.read_bytes(ln).decode(), "golden")
    ln = sint(r, 0, 7)
    eq("wstring (32 bits per char, NO alignment)",
       [r.bits(32) for _ in range(ln)], [0x043C, 0x0438, 0x0440])
    r.align()
    eq("fixed q8.8 (-3.25 in ±100 units)", fixed(r, 8, -100, 100), -(3 * 256 + 64))
    eq("fixed q16.16 (1234.5 in ±2000 units)", fixed(r, 16, -2000, 2000), 1234 * 65536 + 32768)
    eq("fixed q48.16 (in ±100000 units)", fixed(r, 16, -100000, 100000), -(54321 * 65536 + 12345))
    eq("fixed q16.16 unsigned (every fraction bit set)", fixed(r, 16, 0, 30000), 29999 * 65536 + 65535)
    r.align()
    eq("fixed q112.16 wide (75 bits: the 3-group structure)",
       fixed(r, 16, -(2**57), 2**57), -(98765432109 * 65536 + 4321))
    eq("fixed q64.64 wide (128 bits: the 4-group structure)",
       fixed(r, 64, -(2**63), 2**63 - 1), (0x0123456789ABCDEF << 64) + 0x0FEDCBA987654321)
    eq("consumed exactly the golden bytes", math.ceil(r.i / 8), len(data))

    # STANDARD.md, 'uint128': 128 raw bits, low 64-bit half first, each half as
    # serialize_bits( half, 64 ). Verified against the library's second, additive pin.
    u = BitReader(hex_array("golden_uint128_bytes"))
    lo = u.bits(32) | (u.bits(32) << 32)
    hi = u.bits(32) | (u.bits(32) << 32)
    eq("uint128 (low half first, little endian bytes)",
       (hi << 64) | lo, 0x0123456789ABCDEF_FEDCBA9876543210)

    # STANDARD.md, 'int128 (ranged)': offset encoding over the unsigned 128-bit
    # domain in 32-bit groups. Bounds of +/- 2^70 need 72 bits, so this pin
    # load-bears the THREE-group structure — 32, 32, then an 8-bit remainder.
    s = BitReader(hex_array("golden_int128_bytes"))
    eq("int128 ranged (3-group structure, +/- 2^70 bounds)",
       ranged128(s, -(1 << 70), 1 << 70), -0x0123456789ABCDEF)
    eq("int128 ranged consumed exactly 72 bits", s.i, 72)

    # ---- int_relative tier boundaries -------------------------------------
    #
    # These exist because this tool reported "29 checks, 0 failures" for months
    # while `relative` was missing the [4378, 69914] tier AND adding `prev` to
    # the absolute form. The golden stream never reaches those tiers, so no
    # amount of running it could have caught either. A vector that cannot fail
    # is not evidence -- so here are the streams that CAN.
    #
    # Every tier is exercised at BOTH ends, since an off-by-one in a bound
    # shows only at the boundary. Each stream was produced by encoding the
    # stated difference from previous=100 and is decoded here by the document's
    # ladder alone.
    # The streams below were EMITTED BY AN IMPLEMENTATION and are decoded here
    # by the document's ladder alone. That direction matters: an encoder written
    # in this file would share whatever misreading the decoder has and the two
    # would agree with each other forever. Written by serialize.c from
    # previous = 100, one stream per tier boundary.
    for diff, stream_hex, expected in [
        (1,     "01",           101),      # tier 1, exactly 1 -- one bit
        (2,     "02",           102),      # tier 2 low
        (6,     "12",           106),      # tier 2 high
        (7,     "04",           107),      # tier 3 low
        (23,    "84",           123),      # tier 3 high
        (24,    "0800",         124),      # tier 4 low
        (280,   "0810",         380),      # tier 4 high
        (281,   "100000",       381),      # tier 5 low
        (4377,  "100002",       4477),     # tier 5 high
        (4378,  "200000",       4478),     # tier 6 low  -- the tier this tool lacked
        (69914, "200040",       70014),    # tier 6 high -- decoded as 131173 before
        (69915, "c05f440000",   70015),    # absolute    -- decoded as 140130 before
    ]:
        try:
            got = relative(BitReader(bytes.fromhex(stream_hex)), 100)
        except (ValueError, IndexError) as e:
            # IndexError matters as much as a wrong answer: a ladder missing a
            # tier runs off the end of a short stream rather than mis-decoding
            # it, and that must read as a FAILURE here, not a traceback.
            got = "%s: %s" % (type(e).__name__, e)
        eq("int_relative difference %d (tier boundary)" % diff, got, expected)

    print(f"{n} checks against STANDARD.md, {len(fails)} failures")
    for f in fails: print("  FAIL " + f)
    if fails:
        print("\nSTANDARD.md and an implementation disagree. THE IMPLEMENTATION IS THE BUG.")
        return 1
    print("\nEvery pinned vector decodes from STANDARD.md alone.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
