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
// STANDARD.md: "Readers must check that the decoded value lies within
// [min,max] and fail otherwise" — in offset form, reject an offset greater
// than the span. The span computes in uint64 because the full int32 range
// overflows int64 arithmetic's comfort zone in exactly the way that matters.
func sint(r *BitReader, lo, hi int64) (int64, error) {
	n := bitsRequired(lo, hi)
	if n == 0 {
		return lo, nil
	}
	v, err := r.bits(n)
	if err != nil {
		return 0, err
	}
	if v > uint64(hi-lo) {
		return 0, fmt.Errorf("ranged int offset %d exceeds span %d over [%d,%d] — reject, never clamp", v, uint64(hi-lo), lo, hi)
	}
	return int64(v) + lo, nil
}

// ---- compressed_float ------------------------------------------------------
//
// STANDARD.md, "compressed_float": the arithmetic is float32 and the two
// roundings are part of the format — the writer's product rounds to float32
// BEFORE 0.5 is added, the reader's product rounds BEFORE min is added. Go
// permits contracting a float multiply and add into a single FMA unless an
// explicit conversion forces the intermediate rounding — the same permission
// that produced the arm64 divergence this battery exists to catch — so every
// product below passes through an explicit float32(). Do not fold them away.

// compressedFloatParams derives max_integer_value and the wire bit width from
// the range, per the document: values = delta / res clamped to
// [1, 4294967040], max_integer_value = ceil(values).
func compressedFloatParams(min, max, res float32) (uint64, int) {
	delta := max - min
	values := float32(delta / res)
	if !(values >= 1) {
		values = 1
	} else if values > 4294967040 {
		values = 4294967040
	}
	maxIntegerValue := uint64(math.Ceil(float64(values)))
	return maxIntegerValue, bitsRequired(0, int64(maxIntegerValue))
}

// quantizeCompressedFloat is the writer arithmetic: clamp the normalized value
// to [0,1], multiply by max_integer_value, ROUND THE PRODUCT to float32, add
// 0.5, floor. One rounding instead of two writes a different integer for
// values that land between quanta.
func quantizeCompressedFloat(value, min, max, res float32) uint64 {
	maxIntegerValue, _ := compressedFloatParams(min, max, res)
	delta := max - min
	normalized := float32((value - min) / delta)
	if !(normalized >= 0) {
		normalized = 0
	} else if !(normalized <= 1) {
		normalized = 1
	}
	scaled := float32(normalized * float32(maxIntegerValue)) // FIRST rounding: no FMA may cross this line
	return uint64(math.Floor(float64(scaled + 0.5)))         // SECOND rounding, then the floor
}

// decodeCompressedFloat is the reader arithmetic, refusal included:
// STANDARD.md, "Readers must reject an integer greater than
// max_integer_value."
func decodeCompressedFloat(r *BitReader, min, max, res float32) (float32, error) {
	maxIntegerValue, n := compressedFloatParams(min, max, res)
	v, err := r.bits(n)
	if err != nil {
		return 0, err
	}
	if v > maxIntegerValue {
		return 0, fmt.Errorf("compressed_float integer %d exceeds max_integer_value %d", v, maxIntegerValue)
	}
	normalized := float32(float32(v) / float32(maxIntegerValue))
	delta := max - min
	scaled := float32(normalized * delta) // rounds BEFORE min is added; fused, the decode is one ulp off whenever min is non-zero
	return scaled + min, nil
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
	re, err := regexp.Compile(regexp.QuoteMeta(name) + `\[\d*\]\s*=\s*\{([^}]*)\}`)
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

// trailingBits reports the contents of the unused bits of the final byte of a
// stream whose final operation ended at bit position bitIndex — zero for a
// stream written by a conforming writer.
//
// STANDARD.md, "Trailing bits" (adopted 2026-08-15): writers must emit zero
// there, readers must not reject a stream for their contents, and a
// conformance or diagnostic check MAY treat non-zero as evidence the stream
// was not produced by a conforming writer — "was this really written by
// serialize?". This function is that diagnostic. It lives in this tool BY
// RULE: no decode function in this file consults it, and it must never move
// into a read path.
func trailingBits(b []byte, bitIndex int) uint8 {
	if len(b) == 0 || bitIndex%8 == 0 || (bitIndex+7)/8 != len(b) {
		return 0 // ends aligned, or not in the final byte: nothing trails
	}
	return b[len(b)-1] >> uint(bitIndex%8)
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
	// 5.0 over [0,10] lands exactly on a quantum: 0.5 × 1000 = 500 under
	// float32, double and a fused multiply-add alike, so this field is the
	// on-quantum ANCHOR — it keeps the golden stream position honest and can
	// never discriminate. The vectors that can fail are further down.
	if f, err := decodeCompressedFloat(r, 0, 10, 0.01); !c.err("compressed_float", err) {
		c.eq("compressed_float (on-quantum anchor)", f, float32(5.0))
	}
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
	c.eq("wstring (32 bits per UTF-16 code unit, NO alignment)", fmt.Sprint(ws), fmt.Sprint([]uint64{0x043C, 0x0438, 0x0440}))

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

	// Writer obligation (STANDARD.md, "Trailing bits", adopted 2026-08-15):
	// writers must emit zero in the unused bits of the final byte. The golden
	// stream ends 3 bits into its final byte and is the C++ writer's actual
	// emitted output, pinned — so this checks the obligation against a real
	// emission, not against this file's own arithmetic. (The uint128/int128
	// pins end byte-aligned, so the obligation is vacuous there and is not
	// checked: a check that cannot fail is not a check.)
	c.eq("golden_wire_bytes trailing bits are zero (writer obligation)", trailingBits(data, r.i), uint8(0))

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
		br := NewBitReader(raw)
		got, err := relative(br, 100)
		if err != nil {
			// A decode error matters as much as a wrong answer: a ladder
			// missing a tier runs off the end of a short stream rather than
			// mis-decoding it, and that must read as a FAILURE here.
			c.n++
			c.fails = append(c.fails, fmt.Sprintf("%s: %v", name, err))
			continue
		}
		c.eq(name, got, tc.expected)
		// These streams were EMITTED BY serialize.c, so its writer owes the
		// trailing-bits obligation too — and most of them end mid-byte, so
		// the check bites.
		c.eq(name+" trailing bits are zero (writer obligation)", trailingBits(raw, br.i), uint8(0))
	}

	// ---- compressed_float: the discriminating battery ---------------------
	//
	// The golden's compressed_float is 5.0 over [0,10] at res 0.01 — the one
	// value the arm64 divergence could not move. STANDARD.md now says it
	// outright: "Vectors must include values that land between quanta." This
	// battery is imported from the family's discriminating vectors:
	// serialize.c's test/diff3_c.c shape (off-quantum values across several
	// ranges, both boundaries, an on-quantum anchor), serialize.go's
	// brute-forced pair (0.005 and -42.573, compat gate), and the C++
	// library's pinned non-zero-min decode (test_compressed_float_
	// conformance_nonzero_min, serialize#58).

	// Writer quantization: the document's required two-rounding float32
	// arithmetic against what widening or contraction produces. 0.005 catches
	// contraction — an FMA rounds once and writes 0 where the format requires
	// 1; 0.025 / 0.105 / 9.995 catch widening to double (which writes 2, 10
	// and 999); 2.5 is the on-quantum anchor every arithmetic agrees on.
	// The [-100,100] rows run the same arithmetic over a non-zero min,
	// exercising the (value - min) step a zero min turns into a no-op.
	for _, tc := range []struct {
		value, min, max, res float32
		integer              uint64
		note                 string
	}{
		{0.005, 0, 10, 0.01, 1, "half a quantum above min: an FMA writes 0"},
		{0.025, 0, 10, 0.01, 3, "double arithmetic writes 2"},
		{0.105, 0, 10, 0.01, 11, "double arithmetic writes 10"},
		{9.995, 0, 10, 0.01, 1000, "double arithmetic writes 999"},
		{2.5, 0, 10, 0.01, 250, "on-quantum anchor"},
		{-42.573, -100, 100, 0.01, 5743, "off-quantum over a non-zero min"},
		{0.0, -100, 100, 0.01, 10000, "on-quantum sanity row"},
		{-99.875, -100, 100, 0.01, 13, "a double-widened writer quantizes 12"},
		{-33.34, -100, 100, 0.01, 6666, "the arm64 pre-fix value"},
	} {
		got := quantizeCompressedFloat(tc.value, tc.min, tc.max, tc.res)
		c.eq(fmt.Sprintf("compressed_float writer: %v over [%v,%v] at %v (%s)",
			tc.value, tc.min, tc.max, tc.res, tc.note), got, tc.integer)
	}

	// Reader decode with the min non-zero, pinned to exact float32 BITS: the
	// divergence a fused reader produces is one ulp, which no tolerance
	// comparison can see. The stream is the library's additive pin for the
	// non-zero-min case (pinned_bytes, serialize#58), lifted as data like
	// every other vector here: three values over [-100,100] at res 0.01,
	// 15 bits each, then an align.
	if pb, err := hexArray(header, "pinned_bytes"); !c.err("pinned_bytes", err) {
		p := NewBitReader(pb)
		for i, want := range []uint32{0x00000000, 0xC2C7BD71, 0xC2055C2A} {
			if f, err := decodeCompressedFloat(p, -100, 100, 0.01); !c.err("compressed_float non-zero-min decode", err) {
				c.eq(fmt.Sprintf("compressed_float decode %d of pinned_bytes (bit-exact)", i),
					fmt.Sprintf("%08X", math.Float32bits(f)), fmt.Sprintf("%08X", want))
			}
		}
		if !c.err("pinned_bytes align", p.align()) {
			c.eq("pinned_bytes consumed exactly", p.i/8, len(pb))
		}
	}

	// Two more reader pins, streams spelled inline, expectations as hex float
	// literals so the demanded result is bit-identical — the same pins
	// serialize.go carries: decode(1) over [0,10] is float32(1/1000) * 10 + 0
	// and decode(5743) over [-100,100] is float32(5743/20000) * 200 - 100,
	// two roundings each. A fused reader misses the second by one ulp
	// wherever min is non-zero; serialize.go additionally pins integer 384,
	// whose fused decode is 0xC2C051EB against the format's 0xC2C051EC.
	for _, tc := range []struct {
		stream        string
		min, max, res float32
		want          float32
		name          string
	}{
		{"0100", 0, 10, 0.01, 0x1.47ae16p-07, "decode(1) over [0,10]"},
		{"6f16", -100, 100, 0.01, -0x1.548f5cp+05, "decode(5743) over [-100,100]"},
		{"8001", -100, 100, 0.01, -0x1.80a3d8p+06, "decode(384) over [-100,100]"},
	} {
		raw := make([]byte, len(tc.stream)/2)
		for i := range raw {
			b, _ := strconv.ParseUint(tc.stream[i*2:i*2+2], 16, 8)
			raw[i] = byte(b)
		}
		if f, err := decodeCompressedFloat(NewBitReader(raw), tc.min, tc.max, tc.res); !c.err(tc.name, err) {
			c.eq(fmt.Sprintf("compressed_float %s (bit-exact)", tc.name),
				fmt.Sprintf("%08X", math.Float32bits(f)), fmt.Sprintf("%08X", math.Float32bits(tc.want)))
		}
	}

	// ---- refusal paths ----------------------------------------------------
	//
	// STANDARD.md: "An implementation conforms when it reproduces every
	// vector byte for byte AND refuses everything this document says must be
	// refused." Until now this tool exercised only the first half (issue
	// #55). Each stream below is one a conforming reader MUST refuse, decoded
	// by the document's own rules — a decode that SUCCEEDS is the failure.
	//
	// Two refusal classes are deliberately absent, because each turns on a
	// ruling PR #60 leaves open, and a vector here would pre-decide it:
	//   - trailing bits after the final operation: the draft says they cannot
	//     invalidate (no operation reads them); the alternative ruling would
	//     require readers to check them zero, and only then would a refusal
	//     vector exist. Awaiting PR #60.
	//   - reads past the end of the caller's buffer: the draft rules the two
	//     caller memory contracts (over-allocate vs priced window) both
	//     conforming, so there is no single behavior to pin. Awaiting PR #60.

	refuse := func(name string, err error) {
		c.n++
		if err == nil {
			c.fails = append(c.fails, fmt.Sprintf("%s: decode succeeded, the document requires refusal", name))
		}
	}

	// ranged int: [0,10] is 4 bits, so offsets 11..15 are expressible and
	// must be refused — reject, never clamp. 10 is the accept boundary.
	if v, err := sint(NewBitReader([]byte{0x0A}), 0, 10); !c.err("ranged int accept boundary", err) {
		c.eq("ranged int accepts its max (offset == span)", v, int64(10))
	}
	_, err = sint(NewBitReader([]byte{0x0F}), 0, 10)
	refuse("ranged int offset 15 over [0,10]", err)
	_, err = sint(NewBitReader([]byte{0xFF}), -100, 100)
	refuse("ranged int offset 255 over [-100,100]", err)

	// align: padding bits must be zero, and readers must fail the read if
	// they are not. Three bits of value, then five padding bits of ones.
	rp := NewBitReader([]byte{0xFD})
	if _, err := rp.bits(3); err == nil {
		refuse("align with non-zero padding", rp.align())
	}

	// int_relative: the absolute form carries no ordering guarantee of its
	// own, so the reader must check current > previous and fail otherwise.
	// Both streams are the absolute form against previous = 100: one equal,
	// one less.
	for _, tc := range []struct {
		stream string
		note   string
	}{
		{"0019000000", "absolute current == previous"},
		{"800c000000", "absolute current < previous"},
	} {
		raw := make([]byte, len(tc.stream)/2)
		for i := range raw {
			b, _ := strconv.ParseUint(tc.stream[i*2:i*2+2], 16, 8)
			raw[i] = byte(b)
		}
		_, err := relative(NewBitReader(raw), 100)
		refuse(fmt.Sprintf("int_relative %s", tc.note), err)
	}

	// int128 ranged: the decoded offset must be at most max - min in the
	// unsigned domain — reject, never clamp. Bounds of +/- 2^70 span 2^71,
	// 72 bits: offset 2^71 is the accept boundary (decoding to +2^70), and
	// offset 2^71 + 1 must be refused.
	if v, err := ranged128(NewBitReader([]byte{0, 0, 0, 0, 0, 0, 0, 0, 0x80}), new(big.Int).Neg(pow2(70)), pow2(70)); !c.err("int128 accept boundary", err) {
		c.eq("int128 ranged accepts offset == span (decodes +2^70)", v, pow2(70))
	}
	_, err = ranged128(NewBitReader([]byte{0x01, 0, 0, 0, 0, 0, 0, 0, 0x80}), new(big.Int).Neg(pow2(70)), pow2(70))
	refuse("int128 ranged offset span+1", err)

	// fixed: the decoded offset must be at most raw_max - raw_min — reject,
	// never clamp. Q8.8 over [-100,100]: the raw span is 51200 in 16 bits,
	// so 51200 is the accept boundary (decoding to raw +25600) and 65535
	// must be refused.
	if f, err := fixed(NewBitReader([]byte{0x00, 0xC8}), 8, big.NewInt(-100), big.NewInt(100)); !c.err("fixed accept boundary", err) {
		c.eq("fixed q8.8 accepts offset == raw span (decodes +100.0)", f, big.NewInt(25600))
	}
	_, err = fixed(NewBitReader([]byte{0xFF, 0xFF}), 8, big.NewInt(-100), big.NewInt(100))
	refuse("fixed q8.8 offset 65535 over raw span 51200", err)

	// compressed_float: readers must reject an integer greater than
	// max_integer_value. Over [0,10] at res 0.01 that is 1000 in 10 bits:
	// 1000 is the accept boundary (decoding to exactly 10.0), 1001 must be
	// refused.
	if f, err := decodeCompressedFloat(NewBitReader([]byte{0xE8, 0x03}), 0, 10, 0.01); !c.err("compressed_float accept boundary", err) {
		c.eq("compressed_float accepts integer == max_integer_value", f, float32(10.0))
	}
	_, err = decodeCompressedFloat(NewBitReader([]byte{0xE9, 0x03}), 0, 10, 0.01)
	refuse("compressed_float integer 1001 over max_integer_value 1000", err)

	// ---- degenerate ranges: zero bits on every storage width --------------
	//
	// STANDARD.md (adopted 2026-08-15): min == max is legal, costs zero bits,
	// and the reader recovers the value from the range alone — raw min, on
	// every storage width. Decoded from an EMPTY stream, which is the point:
	// any read at all is an error here, so a pass PROVES zero bits. The
	// Q64.64 row is the width where implementations diverged (fraction_bits
	// of zeros on the wide path only).
	empty := NewBitReader(nil)
	if v, err := sint(empty, 42, 42); !c.err("degenerate int", err) {
		c.eq("degenerate int (min == max): value from the range alone", v, int64(42))
	}
	if f, err := fixed(empty, 16, big.NewInt(7), big.NewInt(7)); !c.err("degenerate fixed q48.16", err) {
		c.eq("degenerate fixed q48.16: min << fraction_bits", f, big.NewInt(7*65536))
	}
	if f, err := fixed(empty, 16, big.NewInt(-9), big.NewInt(-9)); !c.err("degenerate fixed negative bounds", err) {
		c.eq("degenerate fixed q112.16 negative bounds: raw min is negative", f, big.NewInt(-9*65536))
	}
	if f, err := fixed(empty, 64, big.NewInt(0), big.NewInt(0)); !c.err("degenerate fixed q64.64", err) {
		c.eq("degenerate fixed q64.64: zero bits, not fraction_bits of zeros", f, big.NewInt(0))
	}
	c.eq("degenerate decodes consumed zero bits", empty.i, 0)

	// ---- trailing bits: the distinction, proven both ways ------------------
	//
	// STANDARD.md (adopted 2026-08-15): writers must write zero; readers must
	// not reject for non-zero; a diagnostic MAY flag non-zero as provenance
	// evidence. Take the one-bit int_relative stream serialize.c emitted as
	// 0x01 and set every trailing bit: 0x81. No conforming writer produces
	// this stream, but its one meaningful bit is untouched, so:
	//   (a) the reader is indifferent — the decode succeeds and yields
	//       exactly what the clean stream yields; a decode that errors here
	//       is a reader rejecting for trailing-bit contents, which the
	//       document forbids;
	//   (b) the diagnostic flags it — trailingBits reports non-zero, the
	//       "was this really written by serialize?" signal.
	doctored := []byte{0x81}
	db := NewBitReader(doctored)
	if got, err := relative(db, 100); !c.err("doctored trailing bits: reader accepts", err) {
		c.eq("doctored trailing bits: reader decodes identically", got, int64(101))
	}
	c.eq("doctored trailing bits: diagnostic flags the stream", trailingBits(doctored, db.i) != 0, true)

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
