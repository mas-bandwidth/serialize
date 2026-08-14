// Command verify_standard checks STANDARD.md against the implementations.
//
// It decodes the library's own golden wire-format vectors using ONLY what
// STANDARD.md states — the bit packing, the ranged-integer widths, the
// alignment rules, the relative-integer ladder, the fixed-point offset
// encoding, the uint128 half order — and asserts every field of the golden
// message plus the additive uint128 and int128 pins.
//
// Nothing here consults an implementation's logic. The golden arrays and the
// expected values are read out of serialize.h AS DATA, which is the point: the
// document is checked against something other than the code it was written
// from. STANDARD.md is the format; where the two disagree, the implementation
// is the bug.
//
//	usage: go run ./tools/conformance
//	exit:  0 = every pinned vector decodes from the document, 1 = it does not
package main

import (
	"fmt"
	"math"
	"math/big"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

// BitReader implements STANDARD.md, "General Conventions": bits are packed
// least-significant-bit first into 64-bit little-endian words.
type BitReader struct {
	b []byte
	i int
}

func NewBitReader(b []byte) *BitReader { return &BitReader{b: b} }

// bits reads n bits LSB-first. It returns an error rather than panicking when
// the stream runs short: a ladder missing a tier runs off the end of a stream
// instead of mis-decoding it, and that must read as a FAILURE, not a crash.
func (r *BitReader) bits(n int) (uint64, error) {
	var v uint64
	for k := 0; k < n; k++ {
		byteIdx := r.i / 8
		if byteIdx >= len(r.b) {
			return 0, fmt.Errorf("read past end of stream at bit %d", r.i)
		}
		v |= uint64((r.b[byteIdx]>>(uint(r.i)%8))&1) << uint(k)
		r.i++
	}
	return v, nil
}

// align consumes padding to the next byte boundary and verifies it is zero.
// STANDARD.md requires readers to reject non-zero padding, which is what makes
// a malformed stream detectable rather than silently accepted.
func (r *BitReader) align() error {
	pad := (8 - (r.i % 8)) % 8
	if pad == 0 {
		return nil
	}
	v, err := r.bits(pad)
	if err != nil {
		return err
	}
	if v != 0 {
		return fmt.Errorf("alignment padding must be zero, got %d", v)
	}
	return nil
}

func (r *BitReader) readBytes(n int) ([]byte, error) {
	if err := r.align(); err != nil {
		return nil, err
	}
	start := r.i / 8
	if start+n > len(r.b) {
		return nil, fmt.Errorf("read of %d bytes past end of stream", n)
	}
	out := r.b[start : start+n]
	r.i += n * 8
	return out, nil
}

// bitsRequired is STANDARD.md's rule verbatim: zero bits for a degenerate
// range, otherwise the bit length of the span.
func bitsRequired(lo, hi int64) int {
	if lo == hi {
		return 0
	}
	return big.NewInt(hi - lo).BitLen()
}

// sint decodes a ranged integer: the offset from lo, in bitsRequired bits.
func sint(r *BitReader, lo, hi int64) (int64, error) {
	n := bitsRequired(lo, hi)
	if n == 0 {
		return lo, nil
	}
	v, err := r.bits(n)
	if err != nil {
		return 0, err
	}
	return int64(v) + lo, nil
}

// relative decodes STANDARD.md's int_relative: a SIX-tier flag ladder, then an
// absolute form.
//
// The Python tool this replaced carried both halves of the same historical bug
// the prose was corrected for months earlier: the [4378, 69914] tier was
// missing entirely, so every stream using it decoded as the absolute form and
// everything after it shifted; and the final tier was decoded as prev + 32 bits
// when it transmits `current` ABSOLUTELY, so it was wrong by exactly prev. It
// decoded 200040 as 131173 instead of 70014, while reporting "29 checks, 0
// failures" — because the golden stream never reaches those tiers. A checker
// that cannot fail is not a checker.
func relative(r *BitReader, prev int64) (int64, error) {
	tiers := []struct{ lo, hi int64 }{
		{2, 6}, {7, 23}, {24, 280}, {281, 4377}, {4378, 69914},
	}

	flag, err := r.bits(1)
	if err != nil {
		return 0, err
	}
	if flag == 1 {
		return prev + 1, nil
	}
	for _, t := range tiers {
		flag, err = r.bits(1)
		if err != nil {
			return 0, err
		}
		if flag == 1 {
			d, err := sint(r, t.lo, t.hi)
			if err != nil {
				return 0, err
			}
			return prev + d, nil
		}
	}
	cur, err := r.bits(32)
	if err != nil {
		return 0, err
	}
	current := int64(cur)
	// The absolute form carries no ordering guarantee of its own, so the
	// reader must check it — STANDARD.md says so in as many words.
	if current <= prev {
		return 0, fmt.Errorf("absolute tier decoded current=%d, not greater than previous=%d", current, prev)
	}
	return current, nil
}

// fixed decodes STANDARD.md's fixed point: an offset encoding over the raw
// (scaled) bounds, written in 32-bit groups from least significant upward.
func fixed(r *BitReader, fractionBits uint, lo, hi *big.Int) (*big.Int, error) {
	rawLo := new(big.Int).Lsh(lo, fractionBits)
	rawHi := new(big.Int).Lsh(hi, fractionBits)
	span := new(big.Int).Sub(rawHi, rawLo)
	n := 0
	if span.Sign() != 0 {
		n = span.BitLen()
	}
	v := new(big.Int)
	for g := 0; g < n; g += 32 {
		w := 32
		if n-g < 32 {
			w = n - g
		}
		grp, err := r.bits(w)
		if err != nil {
			return nil, err
		}
		v.Or(v, new(big.Int).Lsh(new(big.Int).SetUint64(grp), uint(g)))
	}
	if v.Cmp(span) > 0 {
		return nil, fmt.Errorf("fixed offset %s exceeds span %s", v, span)
	}
	return v.Add(v, rawLo), nil
}

// ranged128 decodes STANDARD.md's int128: an offset encoding computed in the
// UNSIGNED 128-bit domain, in 32-bit groups from least significant upward with
// the final group carrying the remainder.
func ranged128(r *BitReader, lo, hi *big.Int) (*big.Int, error) {
	mod := new(big.Int).Lsh(big.NewInt(1), 128)
	mask := func(x *big.Int) *big.Int { return new(big.Int).Mod(x, mod) }
	ulo, uhi := mask(lo), mask(hi)
	span := mask(new(big.Int).Sub(uhi, ulo))
	n := 0
	if span.Sign() != 0 {
		n = span.BitLen()
	}
	v := new(big.Int)
	for g := 0; g < n; g += 32 {
		w := 32
		if n-g < 32 {
			w = n - g
		}
		grp, err := r.bits(w)
		if err != nil {
			return nil, err
		}
		v.Or(v, new(big.Int).Lsh(new(big.Int).SetUint64(grp), uint(g)))
	}
	if v.Cmp(span) > 0 {
		return nil, fmt.Errorf("int128 offset %s exceeds span %s", v, span)
	}
	out := mask(new(big.Int).Add(v, ulo))
	// back to signed two's complement
	if out.Cmp(new(big.Int).Lsh(big.NewInt(1), 127)) >= 0 {
		out.Sub(out, mod)
	}
	return out, nil
}

var hexByte = regexp.MustCompile(`0x([0-9A-Fa-f]{2})`)

// hexArray lifts a pinned vector out of serialize.h as DATA. The tool never
// consults the header's logic, only its constants.
func hexArray(header, name string) ([]byte, error) {
	src, err := os.ReadFile(header)
	if err != nil {
		return nil, err
	}
	re, err := regexp.Compile(regexp.QuoteMeta(name) + `\[\]\s*=\s*\{([^}]*)\}`)
	if err != nil {
		return nil, err
	}
	m := re.FindSubmatch(src)
	if m == nil {
		return nil, fmt.Errorf("%s not found in %s", name, header)
	}
	var out []byte
	for _, h := range hexByte.FindAllSubmatch(m[1], -1) {
		b, err := strconv.ParseUint(string(h[1]), 16, 8)
		if err != nil {
			return nil, err
		}
		out = append(out, byte(b))
	}
	return out, nil
}

type checker struct {
	n     int
	fails []string
}

func (c *checker) eq(name string, got, want interface{}) {
	c.n++
	g, w := fmt.Sprintf("%v", got), fmt.Sprintf("%v", want)
	if g != w {
		c.fails = append(c.fails, fmt.Sprintf("%s: got %s, expected %s", name, g, w))
	}
}

// eqTol compares floats within a tolerance, for the lossy operations.
func (c *checker) eqTol(name string, got, want, tol float64) {
	c.n++
	if math.Abs(got-want) >= tol {
		c.fails = append(c.fails, fmt.Sprintf("%s: got %v, expected %v (tol %v)", name, got, want, tol))
	}
}

// err records a decode failure as a check failure rather than aborting: a
// stream that cannot be decoded is exactly the outcome this tool exists to
// report.
func (c *checker) err(name string, e error) bool {
	if e != nil {
		c.n++
		c.fails = append(c.fails, fmt.Sprintf("%s: %v", name, e))
		return true
	}
	return false
}

func bi(s string) *big.Int {
	v, ok := new(big.Int).SetString(strings.ReplaceAll(s, "_", ""), 0)
	if !ok {
		panic("bad big.Int literal: " + s)
	}
	return v
}

func pow2(n uint) *big.Int { return new(big.Int).Lsh(big.NewInt(1), n) }

func main() {
	exe, err := os.Executable()
	_ = exe
	root, err := filepath.Abs(".")
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	// run from the repo root, or from tools/conformance
	header := filepath.Join(root, "serialize.h")
	if _, err := os.Stat(header); err != nil {
		header = filepath.Join(root, "..", "..", "serialize.h")
	}

	c := &checker{}

	data, err := hexArray(header, "golden_wire_bytes")
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	r := NewBitReader(data)

	rd := func(n int) uint64 {
		v, err := r.bits(n)
		if err != nil {
			c.err("read", err)
		}
		return v
	}
	rs := func(lo, hi int64) int64 {
		v, err := sint(r, lo, hi)
		if err != nil {
			c.err("read", err)
		}
		return v
	}

	// field order per GoldenWireSerialize; values per GoldenWireInit
	c.eq("bits4", rd(4), 13)
	c.eq("bits11", rd(11), 1445)
	c.eq("bits24", rd(24), 11259375)
	c.eq("bits32", rd(32), uint64(0xDEADBEEF))
	c.eq("int_small (ranged -100..100)", rs(-100, 100), -37)
	v := rs(-(1 << 31), (1<<31)-1)
	c.eq("int_full (full int32 range)", v, -123456789)
	c.eq("bool flag", rd(1), 1)
	c.eqTol("float", float64(math.Float32frombits(uint32(rd(32)))), 3.1415926, 1e-6)
	mx := math.Ceil(10.0 / 0.01)
	c.eqTol("compressed_float", float64(rd(bitsRequired(0, int64(mx))))/mx*10.0, 5.0, 1e-4)
	lo64, hi64 := rd(32), rd(32)
	c.eqTol("double", math.Float64frombits(lo64|(hi64<<32)), 1.0/3.0, 1e-12)
	c.eq("uint8", rs(0, 255), 0x7F)
	c.eq("uint16", rs(0, 65535), 0x1234)
	c.eq("uint32", rd(32), uint64(0x12345678))
	lo64, hi64 = rd(32), rd(32)
	c.eq("uint64", lo64|(hi64<<32), uint64(0x123456789ABCDEF0))

	if rel, err := relative(r, 100); !c.err("int_relative near", err) {
		c.eq("int_relative near (difference 1 = one bit)", rel, 101)
	}
	if rel, err := relative(r, 100); !c.err("int_relative far", err) {
		c.eq("int_relative far (difference 2000)", rel, 2100)
	}

	if err := r.align(); !c.err("align before bytes", err) {
		b, err := r.readBytes(7)
		if !c.err("bytes", err) {
			c.eq("bytes", fmt.Sprintf("%x", b), "deadbeefcafe01")
		}
	}
	ln := rs(0, 15)
	if s, err := r.readBytes(int(ln)); !c.err("string", err) {
		c.eq("string", string(s), "golden")
	}
	ln = rs(0, 7)
	var ws []uint64
	for i := int64(0); i < ln; i++ {
		ws = append(ws, rd(32))
	}
	c.eq("wstring (32 bits per char, NO alignment)", fmt.Sprint(ws), fmt.Sprint([]uint64{0x043C, 0x0438, 0x0440}))

	c.err("align before fixed", r.align())
	if f, err := fixed(r, 8, big.NewInt(-100), big.NewInt(100)); !c.err("fixed q8.8", err) {
		c.eq("fixed q8.8 (-3.25 in +/-100 units)", f, big.NewInt(-(3*256 + 64)))
	}
	if f, err := fixed(r, 16, big.NewInt(-2000), big.NewInt(2000)); !c.err("fixed q16.16", err) {
		c.eq("fixed q16.16 (1234.5 in +/-2000 units)", f, big.NewInt(1234*65536+32768))
	}
	if f, err := fixed(r, 16, big.NewInt(-100000), big.NewInt(100000)); !c.err("fixed q48.16", err) {
		c.eq("fixed q48.16 (in +/-100000 units)", f, big.NewInt(-(54321*65536 + 12345)))
	}
	if f, err := fixed(r, 16, big.NewInt(0), big.NewInt(30000)); !c.err("fixed q16.16 unsigned", err) {
		c.eq("fixed q16.16 unsigned (every fraction bit set)", f, big.NewInt(29999*65536+65535))
	}
	c.err("align before wide fixed", r.align())
	if f, err := fixed(r, 16, new(big.Int).Neg(pow2(57)), pow2(57)); !c.err("fixed q112.16", err) {
		want := new(big.Int).Neg(new(big.Int).Add(new(big.Int).Mul(big.NewInt(98765432109), big.NewInt(65536)), big.NewInt(4321)))
		c.eq("fixed q112.16 wide (75 bits: the 3-group structure)", f, want)
	}
	if f, err := fixed(r, 64, new(big.Int).Neg(pow2(63)), new(big.Int).Sub(pow2(63), big.NewInt(1))); !c.err("fixed q64.64", err) {
		want := new(big.Int).Add(new(big.Int).Lsh(bi("0x0123456789ABCDEF"), 64), bi("0x0FEDCBA987654321"))
		c.eq("fixed q64.64 wide (128 bits: the 4-group structure)", f, want)
	}
	c.eq("consumed exactly the golden bytes", (r.i+7)/8, len(data))

	// STANDARD.md, 'uint128': 128 raw bits, low 64-bit half first, each half as
	// serialize_bits( half, 64 ). Verified against the library's additive pin.
	if ub, err := hexArray(header, "golden_uint128_bytes"); !c.err("golden_uint128_bytes", err) {
		u := NewBitReader(ub)
		l1, _ := u.bits(32)
		l2, _ := u.bits(32)
		h1, _ := u.bits(32)
		h2, _ := u.bits(32)
		got := new(big.Int).Add(
			new(big.Int).Lsh(new(big.Int).SetUint64(h1|(h2<<32)), 64),
			new(big.Int).SetUint64(l1|(l2<<32)))
		c.eq("uint128 (low half first, little endian bytes)", got, bi("0x0123456789ABCDEFFEDCBA9876543210"))
	}

	// STANDARD.md, 'int128 (ranged)': bounds of +/- 2^70 need 72 bits, so this
	// pin load-bears the THREE-group structure — 32, 32, then an 8-bit remainder.
	if sb, err := hexArray(header, "golden_int128_bytes"); !c.err("golden_int128_bytes", err) {
		s := NewBitReader(sb)
		if got, err := ranged128(s, new(big.Int).Neg(pow2(70)), pow2(70)); !c.err("int128 ranged", err) {
			c.eq("int128 ranged (3-group structure, +/- 2^70 bounds)", got, bi("-0x0123456789ABCDEF"))
			c.eq("int128 ranged consumed exactly 72 bits", s.i, 72)
		}
	}

	// ---- int_relative tier boundaries -------------------------------------
	//
	// These exist because the predecessor tool reported "29 checks, 0 failures"
	// for months while its ladder was missing the [4378, 69914] tier AND adding
	// prev to the absolute form. The golden stream never reaches those tiers,
	// so no amount of running it could have caught either.
	//
	// The streams below were EMITTED BY serialize.c and are decoded here by the
	// document's ladder alone. That direction matters: an encoder written in
	// this file would share whatever misreading the decoder has, and the two
	// would agree with each other forever. Every tier is exercised at BOTH
	// ends, because an off-by-one in a bound shows only at the boundary.
	for _, tc := range []struct {
		diff     int64
		stream   string
		expected int64
	}{
		{1, "01", 101},               // tier 1, exactly 1 -- one bit
		{2, "02", 102},               // tier 2 low
		{6, "12", 106},               // tier 2 high
		{7, "04", 107},               // tier 3 low
		{23, "84", 123},              // tier 3 high
		{24, "0800", 124},            // tier 4 low
		{280, "0810", 380},           // tier 4 high
		{281, "100000", 381},         // tier 5 low
		{4377, "100002", 4477},       // tier 5 high
		{4378, "200000", 4478},       // tier 6 low  -- the tier the old tool lacked
		{69914, "200040", 70014},     // tier 6 high -- decoded as 131173 before
		{69915, "c05f440000", 70015}, // absolute    -- decoded as 140130 before
	} {
		raw := make([]byte, len(tc.stream)/2)
		for i := range raw {
			b, _ := strconv.ParseUint(tc.stream[i*2:i*2+2], 16, 8)
			raw[i] = byte(b)
		}
		name := fmt.Sprintf("int_relative difference %d (tier boundary)", tc.diff)
		got, err := relative(NewBitReader(raw), 100)
		if err != nil {
			// A decode error matters as much as a wrong answer: a ladder
			// missing a tier runs off the end of a short stream rather than
			// mis-decoding it, and that must read as a FAILURE here.
			c.n++
			c.fails = append(c.fails, fmt.Sprintf("%s: %v", name, err))
			continue
		}
		c.eq(name, got, tc.expected)
	}

	fmt.Printf("%d checks against STANDARD.md, %d failures\n", c.n, len(c.fails))
	for _, f := range c.fails {
		fmt.Println("  FAIL " + f)
	}
	if len(c.fails) > 0 {
		fmt.Println("\nSTANDARD.md and an implementation disagree. THE IMPLEMENTATION IS THE BUG.")
		os.Exit(1)
	}
	fmt.Println("\nEvery pinned vector decodes from STANDARD.md alone.")
}
