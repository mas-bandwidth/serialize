#!/usr/bin/env python3
"""Check serialize/STANDARD.md against the implementation.

Decodes the library's own golden wire-format vector using ONLY what
STANDARD.md states — the bit packing, the ranged-integer widths, the alignment
rules, the relative-integer ladder — and asserts every field of the golden
message. Nothing here consults serialize.h's implementation; the golden array
and the expected values are read out of the header as data.

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
    """STANDARD.md, 'int_relative': the five-tier flag ladder."""
    if r.bits(1): return prev + 1
    if r.bits(1): return prev + sint(r, 2, 6)
    if r.bits(1): return prev + sint(r, 7, 23)
    if r.bits(1): return prev + sint(r, 24, 280)
    if r.bits(1): return prev + sint(r, 281, 4377)
    return prev + r.bits(32)


def golden_bytes():
    src = open(HDR).read()
    m = re.search(r"golden_wire_bytes\[\]\s*=\s*\{(.*?)\};", src, re.S)
    if not m: raise SystemExit("golden_wire_bytes not found in serialize.h")
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))


def main():
    data = golden_bytes()
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
    eq("consumed exactly the golden bytes", math.ceil(r.i / 8), len(data))

    print(f"{n} checks against STANDARD.md, {len(fails)} failures")
    for f in fails: print("  FAIL " + f)
    if fails:
        print("\nSTANDARD.md and the implementation disagree. One of them is wrong.")
        return 1
    print("\nSTANDARD.md matches the implementation.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
