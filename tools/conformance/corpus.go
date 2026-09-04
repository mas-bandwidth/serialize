// Runs the shared conformance corpus against a decoder, encoder and measure written from
// STANDARD.md alone.
//
// This is the second half of what this tool does. verify_standard.go decodes the library's own
// pinned emissions with a decoder written from the document; this file runs the corpus in
// conformance/ the same way. The two ask different questions and neither replaces the other:
// one asks whether the document describes the bytes the implementations emit, the other asks
// whether the document's own vector files say what the document says.
//
// NOTHING HERE CONSULTS serialize.h. The decoders are the ones verify_standard.go already
// carries — the LSB-first bit packing, the ranged widths, the alignment rules, the relative
// integer ladder, the fixed point offset encoding, the two-rounding compressed float — and the
// encoder and the measure below are written from the same text. That independence is the point:
// a corpus checked only by the implementation it judges proves that the implementation agrees
// with itself, and the family already paid for that mistake once.
//
// A corpus file whose operation this checker cannot drive is a gap in the checker, not a pass:
// such a vector FAILS, exactly as it does in the C++ runner.
package main

import (
	"fmt"
	"math"
	"math/big"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"unicode/utf8"
)

// ---------------------------------------------------------------------------
// the bit writer, which is the reader's mirror: STANDARD.md, "General
// Conventions", bits packed least-significant-bit first.

type BitWriter struct {
	b []byte
	i int
}

func (w *BitWriter) bits(v uint64, n int) {
	for k := 0; k < n; k++ {
		if w.i/8 >= len(w.b) {
			w.b = append(w.b, 0)
		}
		if (v>>uint(k))&1 != 0 {
			w.b[w.i/8] |= 1 << uint(w.i%8)
		}
		w.i++
	}
}

// align pads with ZERO BITS to the next byte boundary, and writes nothing when
// the stream is already aligned.
func (w *BitWriter) align() {
	pad := (8 - (w.i % 8)) % 8
	w.bits(0, pad)
}

func (w *BitWriter) writeBytes(data []byte) {
	w.align()
	for _, c := range data {
		w.bits(uint64(c), 8)
	}
}

// flush rounds the stream up to a whole byte. STANDARD.md, "Trailing bits":
// writers must emit ZERO in the unused bits of the final byte, which this
// writer does by construction because it never sets a bit it was not asked to.
func (w *BitWriter) flush() []byte {
	for w.i%8 != 0 {
		w.bits(0, 1)
	}
	return w.b
}

// ---------------------------------------------------------------------------
// the operations, as steps. One step description drives the decode, the encode
// and the measure, so the three cannot drift apart.

type stepKind int

const (
	stepBits stepKind = iota
	stepBool
	stepUint128
	stepAlign
	stepInt
	stepInt64
	stepInt128
	stepIntRelative
	stepFloat
	stepDouble
	stepCompressedFloat
	stepBytes
	stepString
	stepWString
	stepFixed
	// stepObject wraps the steps that follow it. STANDARD.md, "object": it
	// contributes NO BYTES OF ITS OWN — composition, not an encoding — so the
	// nested run is exactly the steps it wraps and nothing else.
	stepObject
)

type step struct {
	kind         stepKind
	width        int64 // bits, count, buffer_size or preceding_bits
	fractionBits uint
	lo, hi       *big.Int
	fmin, fmax   float32
	fres         float32
	previous     int64

	// decoded outputs
	number *big.Int
	bits   *big.Int
	flag   bool
	data   []byte
	units  []uint16
}

// aligns reports whether the step performs an alignment, which is what the
// measure charges the worst case for: STANDARD.md, "The Measure Stream", names
// align, bytes and string and nothing else.
func (s *step) aligns() bool {
	return s.kind == stepAlign || s.kind == stepBytes || s.kind == stepString
}

// bitsRequiredBig is STANDARD.md's rule at any width: zero for a degenerate
// range, otherwise the bit length of the span.
func bitsRequiredBig(span *big.Int) int {
	if span.Sign() == 0 {
		return 0
	}
	return span.BitLen()
}

// readGroups reads n bits as 32-bit groups from least significant upward, the
// splitting rule serialize_bits, int128 and the wide fixed point path share.
func readGroups(r *BitReader, n int) (*big.Int, error) {
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
	return v, nil
}

func writeGroups(w *BitWriter, v *big.Int, n int) {
	for g := 0; g < n; g += 32 {
		width := 32
		if n-g < 32 {
			width = n - g
		}
		grp := new(big.Int).Rsh(v, uint(g))
		grp.And(grp, new(big.Int).Sub(new(big.Int).Lsh(big.NewInt(1), uint(width)), big.NewInt(1)))
		w.bits(grp.Uint64(), width)
	}
}

var mod128 = new(big.Int).Lsh(big.NewInt(1), 128)

func toUnsigned128(x *big.Int) *big.Int { return new(big.Int).Mod(x, mod128) }

func toSigned128(x *big.Int) *big.Int {
	out := toUnsigned128(x)
	if out.Cmp(new(big.Int).Lsh(big.NewInt(1), 127)) >= 0 {
		out.Sub(out, mod128)
	}
	return out
}

// rangedSpan is the span of a ranged operation, computed in the unsigned domain
// of its width — the thing an implementation subtracting in a signed type gets
// wrong at the widest declarations.
func (s *step) rangedSpan() *big.Int {
	return toUnsigned128(new(big.Int).Sub(s.hi, s.lo))
}

func (s *step) rawBounds() (*big.Int, *big.Int) {
	return new(big.Int).Lsh(s.lo, s.fractionBits), new(big.Int).Lsh(s.hi, s.fractionBits)
}

// ---------------------------------------------------------------------------
// decode

// stepSpan is one step, plus the steps a nested object owns.
func stepSpan(steps []*step, i int) int {
	if steps[i].kind == stepObject {
		return 1 + int(steps[i].width)
	}
	return 1
}

// decodeSteps walks a step list, descending into each nested object. The
// descent is the whole of what serialize_object does: no framing, no length
// prefix and no alignment around it.
func decodeSteps(r *BitReader, steps []*step) (int, error) {
	for i := 0; i < len(steps); i += stepSpan(steps, i) {
		if steps[i].kind == stepObject {
			if j, err := decodeSteps(r, steps[i+1:i+1+int(steps[i].width)]); err != nil {
				return i + 1 + j, err
			}
			continue
		}
		if err := decodeStep(r, steps[i]); err != nil {
			return i, err
		}
	}
	return -1, nil
}

func encodeSteps(w *BitWriter, steps []*step) error {
	for i := 0; i < len(steps); i += stepSpan(steps, i) {
		if steps[i].kind == stepObject {
			if err := encodeSteps(w, steps[i+1:i+1+int(steps[i].width)]); err != nil {
				return err
			}
			continue
		}
		if err := encodeStep(w, steps[i]); err != nil {
			return err
		}
	}
	return nil
}

func decodeStep(r *BitReader, s *step) error {
	switch s.kind {
	case stepBits:
		v, err := readGroups(r, int(s.width))
		if err != nil {
			return err
		}
		s.bits = v
		return nil

	case stepBool:
		v, err := r.bits(1)
		if err != nil {
			return err
		}
		s.flag = v == 1
		return nil

	case stepUint128:
		lo, err := readGroups(r, 64)
		if err != nil {
			return err
		}
		hi, err := readGroups(r, 64)
		if err != nil {
			return err
		}
		s.bits = new(big.Int).Or(new(big.Int).Lsh(hi, 64), lo)
		return nil

	case stepAlign:
		return r.align()

	case stepInt, stepInt64, stepInt128:
		span := s.rangedSpan()
		n := bitsRequiredBig(span)
		v, err := readGroups(r, n)
		if err != nil {
			return err
		}
		if v.Cmp(span) > 0 {
			return fmt.Errorf("ranged offset %s exceeds span %s — reject, never clamp", v, span)
		}
		s.number = toSigned128(new(big.Int).Add(v, s.lo))
		return nil

	case stepIntRelative:
		v, err := relative(r, s.previous)
		if err != nil {
			return err
		}
		s.number = big.NewInt(v)
		return nil

	case stepFloat:
		v, err := r.bits(32)
		if err != nil {
			return err
		}
		s.bits = new(big.Int).SetUint64(v)
		return nil

	case stepDouble:
		v, err := readGroups(r, 64)
		if err != nil {
			return err
		}
		s.bits = v
		return nil

	case stepCompressedFloat:
		f, err := decodeCompressedFloat(r, s.fmin, s.fmax, s.fres)
		if err != nil {
			return err
		}
		s.bits = new(big.Int).SetUint64(uint64(math.Float32bits(f)))
		return nil

	case stepBytes:
		b, err := r.readBytes(int(s.width))
		if err != nil {
			return err
		}
		s.data = append([]byte(nil), b...)
		return nil

	case stepString:
		n, err := decodeRangedInt(r, 0, s.width-1)
		if err != nil {
			return err
		}
		b, err := r.readBytes(int(n))
		if err != nil {
			return err
		}
		// STANDARD.md: invalid UTF-8 fails the read, and so does an interior
		// NUL — a zero byte anywhere among the transmitted bytes.
		if !utf8.Valid(b) {
			return fmt.Errorf("string payload is not well-formed UTF-8")
		}
		for _, c := range b {
			if c == 0 {
				return fmt.Errorf("string payload carries an interior NUL")
			}
		}
		s.data = append([]byte(nil), b...)
		return nil

	case stepWString:
		n, err := decodeRangedInt(r, 0, s.width-1)
		if err != nil {
			return err
		}
		units := make([]uint16, 0, n)
		for i := int64(0); i < n; i++ {
			g, err := r.bits(32)
			if err != nil {
				return err
			}
			// a group above 0xFFFF is not a UTF-16 code unit, and the reader
			// must refuse it on every platform
			if g > 0xFFFF {
				return fmt.Errorf("wstring group 0x%X is above 0xFFFF and is not a code unit", g)
			}
			if g == 0 {
				return fmt.Errorf("wstring payload carries an interior NUL group")
			}
			units = append(units, uint16(g))
		}
		if err := checkUTF16(units); err != nil {
			return err
		}
		s.units = units
		return nil

	case stepFixed:
		rawLo, rawHi := s.rawBounds()
		span := new(big.Int).Sub(rawHi, rawLo)
		n := bitsRequiredBig(span)
		v, err := readGroups(r, n)
		if err != nil {
			return err
		}
		if v.Cmp(span) > 0 {
			return fmt.Errorf("fixed offset %s exceeds raw span %s — reject, never clamp", v, span)
		}
		s.number = new(big.Int).Add(v, rawLo)
		return nil
	}
	return fmt.Errorf("no decoder for this step")
}

// decodeRangedInt is the length field shared by string and wstring.
func decodeRangedInt(r *BitReader, lo, hi int64) (int64, error) {
	return sint(r, lo, hi)
}

// checkUTF16 applies STANDARD.md's unpaired-surrogate rule: a high surrogate
// not immediately followed by a low one, a low surrogate not immediately
// preceded by a high one, or a high surrogate as the final transmitted group.
func checkUTF16(units []uint16) error {
	for i := 0; i < len(units); i++ {
		u := units[i]
		if u >= 0xD800 && u <= 0xDBFF {
			if i+1 >= len(units) {
				return fmt.Errorf("wstring ends on a high surrogate 0x%04X", u)
			}
			next := units[i+1]
			if next < 0xDC00 || next > 0xDFFF {
				return fmt.Errorf("wstring high surrogate 0x%04X is not followed by a low surrogate", u)
			}
			i++
			continue
		}
		if u >= 0xDC00 && u <= 0xDFFF {
			return fmt.Errorf("wstring low surrogate 0x%04X is not preceded by a high surrogate", u)
		}
	}
	return nil
}

// ---------------------------------------------------------------------------
// encode, for the vectors carrying `writer = canonical`

func encodeStep(w *BitWriter, s *step) error {
	switch s.kind {
	case stepBits:
		writeGroups(w, s.bits, int(s.width))
	case stepBool:
		if s.flag {
			w.bits(1, 1)
		} else {
			w.bits(0, 1)
		}
	case stepUint128:
		lowMask := new(big.Int).Sub(new(big.Int).Lsh(big.NewInt(1), 64), big.NewInt(1))
		writeGroups(w, new(big.Int).And(s.bits, lowMask), 64)
		writeGroups(w, new(big.Int).Rsh(s.bits, 64), 64)
	case stepAlign:
		w.align()
	case stepInt, stepInt64, stepInt128:
		span := s.rangedSpan()
		writeGroups(w, toUnsigned128(new(big.Int).Sub(s.number, s.lo)), bitsRequiredBig(span))
	case stepIntRelative:
		encodeRelative(w, s.previous, s.number.Int64())
	case stepFloat:
		w.bits(s.bits.Uint64(), 32)
	case stepDouble:
		writeGroups(w, s.bits, 64)
	case stepCompressedFloat:
		f := math.Float32frombits(uint32(s.bits.Uint64()))
		_, n := compressedFloatParams(s.fmin, s.fmax, s.fres)
		w.bits(quantizeCompressedFloat(f, s.fmin, s.fmax, s.fres), n)
	case stepBytes:
		w.writeBytes(s.data)
	case stepString:
		writeGroups(w, big.NewInt(int64(len(s.data))), bitsRequiredBig(big.NewInt(s.width-1)))
		w.writeBytes(s.data)
	case stepWString:
		writeGroups(w, big.NewInt(int64(len(s.units))), bitsRequiredBig(big.NewInt(s.width-1)))
		for _, u := range s.units {
			w.bits(uint64(u), 32)
		}
	case stepFixed:
		rawLo, rawHi := s.rawBounds()
		span := new(big.Int).Sub(rawHi, rawLo)
		writeGroups(w, new(big.Int).Sub(s.number, rawLo), bitsRequiredBig(span))
	default:
		return fmt.Errorf("no encoder for this step")
	}
	return nil
}

// relativeTiers is STANDARD.md's ladder, in order. The writer takes the first tier
// the difference fits, and that encoding is the canonical one.
var relativeTiers = []struct{ lo, hi int64 }{{2, 6}, {7, 23}, {24, 280}, {281, 4377}, {4378, 69914}}

// relativeBits is the width of the encoding encodeRelative emits, which the measure
// charges exactly: nothing in this operation depends on the bit position it starts at.
func relativeBits(prev, current int64) int {
	d := current - prev
	if d == 1 {
		return 1
	}
	for i, t := range relativeTiers {
		if d >= t.lo && d <= t.hi {
			return i + 1 + 1 + bitsRequired(t.lo, t.hi)
		}
	}
	return 6 + 32
}

// encodeRelative emits the FIRST tier the difference fits, which is the
// canonical encoding STANDARD.md's ladder defines.
func encodeRelative(w *BitWriter, prev, current int64) {
	d := current - prev
	if d == 1 {
		w.bits(1, 1)
		return
	}
	for i, t := range relativeTiers {
		if d >= t.lo && d <= t.hi {
			w.bits(0, i+1)
			w.bits(1, 1)
			w.bits(uint64(d-t.lo), bitsRequired(t.lo, t.hi))
			return
		}
	}
	w.bits(0, 6)
	w.bits(uint64(uint32(current)), 32)
}

// ---------------------------------------------------------------------------
// measure. STANDARD.md makes a measure a BOUND: it must report a size
// sufficient to serialize the message AT ANY STARTING BIT POSITION, and
// exact-from-zero accounting is non-conforming precisely because it
// under-counts every unaligned start.
//
// The corpus states a floor. This computes the true worst case — the maximum,
// over every starting bit position, of the bits the message actually occupies —
// and requires the corpus floor to be exactly it. That checks the floor from
// the document rather than from either implementation's arithmetic, and it is
// the check that would catch a corpus floor set to the exact-from-zero number.

func stepExactBits(s *step, bitIndex int) int {
	switch s.kind {
	case stepBits:
		return int(s.width)
	case stepBool:
		return 1
	case stepUint128:
		return 128
	case stepAlign:
		return (8 - (bitIndex % 8)) % 8
	case stepInt, stepInt64, stepInt128:
		return bitsRequiredBig(s.rangedSpan())
	case stepFloat:
		return 32
	case stepDouble:
		return 64
	case stepCompressedFloat:
		_, n := compressedFloatParams(s.fmin, s.fmax, s.fres)
		return n
	case stepIntRelative:
		return relativeBits(s.previous, s.number.Int64())
	case stepBytes:
		return (8-(bitIndex%8))%8 + 8*len(s.data)
	case stepString:
		n := bitsRequiredBig(big.NewInt(s.width - 1))
		return n + (8-((bitIndex+n)%8))%8 + 8*len(s.data)
	case stepWString:
		return bitsRequiredBig(big.NewInt(s.width-1)) + 32*len(s.units)
	case stepFixed:
		rawLo, rawHi := s.rawBounds()
		return bitsRequiredBig(new(big.Int).Sub(rawHi, rawLo))
	case stepObject:
		return 0
	}
	return 0
}

func worstCaseBits(steps []*step) int {
	worst := 0
	for start := 0; start < 8; start++ {
		index := start
		for _, s := range steps {
			// an object contributes no bits of its own; the steps it wraps are
			// already in this list and each is charged once
			index += stepExactBits(s, index)
		}
		if index-start > worst {
			worst = index - start
		}
	}
	return worst
}

// ---------------------------------------------------------------------------
// vector files

type vector struct {
	file        string
	operation   string
	name        string
	params      map[string]string
	stepText    []string
	bytes       []byte
	expectKind  string // "refused", "value", "bits"
	expect      string
	consumed    int64
	hasCons     bool
	measureMin  int64
	hasMeasure  bool
	writerCanon bool
}

func parseVectorFile(path string) ([]*vector, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var out []*vector
	cur := &vector{file: path, params: map[string]string{}, expectKind: "value"}
	flush := func() {
		if cur.operation != "" {
			out = append(out, cur)
		}
		cur = &vector{file: path, params: map[string]string{}, expectKind: "value"}
	}
	for _, line := range strings.Split(string(raw), "\n") {
		// STANDARD.md, "Lexical rules": a comment begins at the start of a line
		// and nowhere else.
		if strings.HasPrefix(line, "#") {
			continue
		}
		text := strings.TrimSpace(line)
		if text == "" {
			flush()
			continue
		}
		key := text
		value := ""
		if i := strings.IndexByte(text, ' '); i >= 0 {
			key, value = text[:i], strings.TrimSpace(text[i+1:])
		}
		switch key {
		case "operation":
			cur.operation = value
		case "name":
			cur.name = value
		case "param":
			eq := strings.IndexByte(value, '=')
			if eq < 0 {
				return nil, fmt.Errorf("%s: malformed param line %q", path, text)
			}
			pn := strings.TrimSpace(value[:eq])
			pv := strings.TrimSpace(value[eq+1:])
			if pn == "step" {
				cur.stepText = append(cur.stepText, pv)
			} else {
				cur.params[pn] = pv
			}
		case "bytes":
			b, err := parseHexBytes(value)
			if err != nil {
				return nil, fmt.Errorf("%s: %v", path, err)
			}
			cur.bytes = b
		case "expect":
			if value == "refused" {
				cur.expectKind = "refused"
				break
			}
			eq := strings.IndexByte(value, '=')
			if eq < 0 {
				return nil, fmt.Errorf("%s: malformed expect line %q", path, text)
			}
			cur.expectKind = strings.TrimSpace(value[:eq])
			cur.expect = strings.TrimSpace(value[eq+1:])
			if cur.expectKind != "value" && cur.expectKind != "bits" {
				return nil, fmt.Errorf("%s: unknown expect kind %q", path, cur.expectKind)
			}
		case "consumed":
			n, err := strconv.ParseInt(value, 10, 64)
			if err != nil {
				return nil, fmt.Errorf("%s: %v", path, err)
			}
			cur.consumed, cur.hasCons = n, true
		case "measure_at_least":
			n, err := strconv.ParseInt(value, 10, 64)
			if err != nil {
				return nil, fmt.Errorf("%s: %v", path, err)
			}
			cur.measureMin, cur.hasMeasure = n, true
		case "writer":
			if value != "canonical" {
				return nil, fmt.Errorf("%s: unknown writer mode %q", path, value)
			}
			cur.writerCanon = true
		default:
			return nil, fmt.Errorf("%s: unknown key %q", path, key)
		}
	}
	flush()
	return out, nil
}

func parseHexBytes(text string) ([]byte, error) {
	var out []byte
	for _, f := range strings.Fields(text) {
		if len(f)%2 != 0 {
			return nil, fmt.Errorf("malformed hexadecimal %q", f)
		}
		for i := 0; i < len(f); i += 2 {
			v, err := strconv.ParseUint(f[i:i+2], 16, 8)
			if err != nil {
				return nil, err
			}
			out = append(out, byte(v))
		}
	}
	return out, nil
}

// numbers are signed decimal or 0x hexadecimal, up to 128 bits wide
func parseNumber(text string) (*big.Int, bool) {
	v, ok := new(big.Int).SetString(strings.TrimSpace(text), 0)
	return v, ok
}

// ---------------------------------------------------------------------------
// building steps

func buildStep(spec string) (*step, error) {
	words := strings.Fields(spec)
	if len(words) == 0 {
		return nil, fmt.Errorf("empty step")
	}
	s := &step{}
	num := func(i int) int64 {
		n, _ := parseNumber(words[i])
		return n.Int64()
	}
	switch words[0] {
	case "bits":
		s.kind, s.width = stepBits, num(1)
	case "bool":
		s.kind = stepBool
	case "align":
		s.kind = stepAlign
	case "object":
		s.kind, s.width = stepObject, num(1)
	case "float":
		s.kind = stepFloat
	case "bytes":
		s.kind, s.width = stepBytes, num(1)
	case "string":
		s.kind, s.width = stepString, num(1)
	case "wstring":
		s.kind, s.width = stepWString, num(1)
	case "double":
		s.kind = stepDouble
	case "uint128":
		s.kind = stepUint128
	case "int", "int64", "int128":
		lo, _ := parseNumber(words[1])
		hi, _ := parseNumber(words[2])
		s.lo, s.hi = lo, hi
		switch words[0] {
		case "int":
			s.kind = stepInt
		case "int64":
			s.kind = stepInt64
		default:
			s.kind = stepInt128
		}
	case "int_relative":
		s.kind, s.previous = stepIntRelative, num(1)
	case "compressed_float":
		f := func(i int) float32 {
			v, _ := strconv.ParseFloat(words[i], 32)
			return float32(v)
		}
		s.kind, s.fmin, s.fmax, s.fres = stepCompressedFloat, f(1), f(2), f(3)
	case "fixed":
		lo, _ := parseNumber(words[3])
		hi, _ := parseNumber(words[4])
		s.kind, s.fractionBits, s.lo, s.hi = stepFixed, uint(num(2)), lo, hi
	default:
		return nil, fmt.Errorf("no runner for step %q", spec)
	}
	return s, nil
}

func buildSteps(v *vector) ([]*step, error) {
	if v.operation == "sequence" {
		var out []*step
		for _, spec := range v.stepText {
			s, err := buildStep(spec)
			if err != nil {
				return nil, err
			}
			out = append(out, s)
		}
		if len(out) == 0 {
			return nil, fmt.Errorf("a sequence with no steps")
		}
		return out, nil
	}
	if len(v.stepText) > 0 {
		return nil, fmt.Errorf("steps are only meaningful on a sequence")
	}

	var out []*step
	if p, ok := v.params["preceding_bits"]; ok {
		n, valid := parseNumber(p)
		if !valid {
			return nil, fmt.Errorf("malformed preceding_bits")
		}
		if n.Sign() > 0 {
			out = append(out, &step{kind: stepBits, width: n.Int64()})
		}
	}

	s := &step{}
	numParam := func(name string) (*big.Int, error) {
		p, ok := v.params[name]
		if !ok {
			return nil, fmt.Errorf("missing parameter %q", name)
		}
		n, valid := parseNumber(p)
		if !valid {
			return nil, fmt.Errorf("malformed parameter %q", name)
		}
		return n, nil
	}
	floatParam := func(name string) (float32, error) {
		p, ok := v.params[name]
		if !ok {
			return 0, fmt.Errorf("missing parameter %q", name)
		}
		f, err := strconv.ParseFloat(p, 32)
		return float32(f), err
	}
	bounds := func() error {
		lo, err := numParam("min")
		if err != nil {
			return err
		}
		hi, err := numParam("max")
		if err != nil {
			return err
		}
		s.lo, s.hi = lo, hi
		return nil
	}

	switch v.operation {
	case "bits":
		n, err := numParam("bits")
		if err != nil {
			return nil, err
		}
		s.kind, s.width = stepBits, n.Int64()
	case "bool":
		s.kind = stepBool
	case "uint128":
		s.kind = stepUint128
	case "align":
		s.kind = stepAlign
	case "int", "int64", "int128":
		s.kind = map[string]stepKind{"int": stepInt, "int64": stepInt64, "int128": stepInt128}[v.operation]
		if err := bounds(); err != nil {
			return nil, err
		}
	case "int_relative":
		n, err := numParam("previous")
		if err != nil {
			return nil, err
		}
		s.kind, s.previous = stepIntRelative, n.Int64()
	case "float":
		s.kind = stepFloat
	case "double":
		s.kind = stepDouble
	case "compressed_float":
		var err error
		s.kind = stepCompressedFloat
		if s.fmin, err = floatParam("min"); err != nil {
			return nil, err
		}
		if s.fmax, err = floatParam("max"); err != nil {
			return nil, err
		}
		if s.fres, err = floatParam("res"); err != nil {
			return nil, err
		}
	case "bytes":
		n, err := numParam("count")
		if err != nil {
			return nil, err
		}
		s.kind, s.width = stepBytes, n.Int64()
	case "string", "wstring":
		n, err := numParam("buffer_size")
		if err != nil {
			return nil, err
		}
		s.kind, s.width = stepString, n.Int64()
		if v.operation == "wstring" {
			s.kind = stepWString
		}
	case "fixed":
		fb, err := numParam("fraction_bits")
		if err != nil {
			return nil, err
		}
		if _, err := numParam("integer_bits"); err != nil {
			return nil, err
		}
		s.kind, s.fractionBits = stepFixed, uint(fb.Int64())
		if err := bounds(); err != nil {
			return nil, err
		}
	default:
		return nil, fmt.Errorf("no runner for operation %q", v.operation)
	}
	return append(out, s), nil
}

// ---------------------------------------------------------------------------
// rendering and comparison. Numeric values are compared as 128 bit two's
// complement PATTERNS, which makes a hexadecimal expectation and its decimal
// twin one expectation and keeps float, double and compressed_float away from
// any value-space comparison — STANDARD.md requires those to be compared as bit
// patterns, because NaN is unequal to itself and -0.0 equals 0.0.

func stepPattern(s *step) (*big.Int, bool) {
	switch s.kind {
	case stepBits, stepUint128, stepFloat, stepDouble, stepCompressedFloat:
		return toUnsigned128(s.bits), true
	case stepInt, stepInt64, stepInt128, stepIntRelative, stepFixed:
		return toUnsigned128(s.number), true
	}
	return nil, false
}

func renderStep(s *step) string {
	if p, ok := stepPattern(s); ok {
		return fmt.Sprintf("0x%032X", p)
	}
	switch s.kind {
	case stepAlign, stepObject:
		return "0"
	case stepBool:
		if s.flag {
			return "true"
		}
		return "false"
	case stepBytes, stepString:
		parts := make([]string, len(s.data))
		for i, c := range s.data {
			parts[i] = fmt.Sprintf("%02X", c)
		}
		return strings.Join(parts, " ")
	case stepWString:
		parts := make([]string, len(s.units))
		for i, u := range s.units {
			parts[i] = fmt.Sprintf("%04X", u)
		}
		return strings.Join(parts, " ")
	}
	return "?"
}

func stepMatches(s *step, expected string) bool {
	if p, ok := stepPattern(s); ok {
		want, valid := parseNumber(expected)
		if !valid {
			return false
		}
		return p.Cmp(toUnsigned128(want)) == 0
	}
	return renderStep(s) == expected
}

// ---------------------------------------------------------------------------
// running

func runCorpusVector(c *checker, v *vector) {
	c.n++
	fail := func(format string, args ...interface{}) {
		c.fails = append(c.fails, fmt.Sprintf("%s: %s [%s]", v.name, fmt.Sprintf(format, args...), v.file))
	}

	steps, err := buildSteps(v)
	if err != nil {
		fail("%v", err)
		return
	}

	// STANDARD.md: "A harness presents every stream with the slack the contract
	// requires." The slack is filled with a non-zero pattern, so a decode that
	// depends on memory past the end cannot pass by reading zeros.
	stream := append(append([]byte(nil), v.bytes...), make([]byte, 8)...)
	for i := len(v.bytes); i < len(stream); i++ {
		stream[i] = 0xA5
	}
	r := &BitReader{b: stream[:len(v.bytes)]}

	failedStep, _ := decodeSteps(r, steps)

	if v.expectKind == "refused" {
		if failedStep < 0 {
			fail("the decode succeeded, the corpus requires refusal")
			return
		}
		// failure is terminal: every step after the failing one must fail too.
		// This decoder has no stream object to latch, so the rule is enforced
		// by construction — the position is not defined past a failure and the
		// checker does not continue on it, which is one of the two shapes
		// STANDARD.md admits.
		return
	}
	if failedStep >= 0 {
		fail("step %d was refused, the corpus requires the read to be accepted", failedStep+1)
		return
	}

	entries := strings.Split(v.expect, "|")
	for i := range entries {
		entries[i] = strings.TrimSpace(entries[i])
	}
	offset := len(steps) - len(entries)
	if offset < 0 {
		fail("the expect list states more values than the vector has steps")
		return
	}
	for i, want := range entries {
		if want == "-" {
			continue
		}
		if !stepMatches(steps[offset+i], want) {
			fail("step %d decoded %s, the corpus states %s", offset+i+1, renderStep(steps[offset+i]), want)
			return
		}
	}

	if v.hasCons && int64(r.i) != v.consumed {
		fail("consumed %d bits, the corpus states %d", r.i, v.consumed)
		return
	}

	if v.writerCanon {
		w := &BitWriter{}
		if err := encodeSteps(w, steps); err != nil {
			fail("%v", err)
			return
		}
		got := w.flush()
		if fmt.Sprintf("% X", got) != fmt.Sprintf("% X", v.bytes) {
			fail("the writer emitted % X, the corpus states % X", got, v.bytes)
			return
		}
	}

	if v.hasMeasure {
		// the corpus floor must be the true worst case over every starting bit
		// position: a floor set to the exact-from-zero number is exactly the
		// non-conforming accounting STANDARD.md names
		if worst := int64(worstCaseBits(steps)); worst != v.measureMin {
			fail("measure_at_least states %d, the document's worst case over every starting bit position is %d", v.measureMin, worst)
		}
	}
}

// runCorpus runs every vector in the corpus. With no arguments it walks the
// repository's conformance/ directory; named files or directories replace it,
// which is how the negative control in conformance-controls/ is run — the
// control must turn this checker RED, and a run that skipped it would be
// reporting green over a rule nobody checked.
func runCorpus(c *checker, root string) {
	var files []string
	if args := os.Args[1:]; len(args) > 0 {
		for _, arg := range args {
			info, err := os.Stat(arg)
			if err != nil {
				c.n++
				c.fails = append(c.fails, err.Error())
				continue
			}
			if info.IsDir() {
				found, _ := filepath.Glob(filepath.Join(arg, "*.txt"))
				files = append(files, found...)
			} else {
				files = append(files, arg)
			}
		}
		runCorpusFiles(c, files)
		return
	}

	dir := filepath.Join(root, "conformance")
	if _, err := os.Stat(dir); err != nil {
		dir = filepath.Join(root, "..", "..", "conformance")
	}
	found, err := filepath.Glob(filepath.Join(dir, "*.txt"))
	if err != nil || len(found) == 0 {
		c.n++
		c.fails = append(c.fails, "the corpus holds no vector files. The corpus is the conformance instrument; an empty one is a broken checkout, not a pass.")
		return
	}
	runCorpusFiles(c, found)
}

func runCorpusFiles(c *checker, files []string) {
	sort.Strings(files)
	for _, f := range files {
		vectors, err := parseVectorFile(f)
		if err != nil {
			c.n++
			c.fails = append(c.fails, err.Error())
			continue
		}
		for _, v := range vectors {
			runCorpusVector(c, v)
		}
	}
}
