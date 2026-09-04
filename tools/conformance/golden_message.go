package main

import (
	"fmt"
	"math"
	"math/big"
	"os"
	"regexp"
	"strings"
)

// The golden message, as a step list built from STANDARD.md's rules alone.
//
// STANDARD.md's Worked Example prints this message's 112 bytes, and this is where that
// claim is proved. The steps below are the operation sequence and the values, and the
// bytes come out of the corpus encoder in corpus.go, which is written from the document
// and knows nothing about serialize.h. Three things are then required to agree: the bytes
// the document's own encoder produces, the bytes the C++ writer emitted, which the library
// pins as golden_wire_bytes, and the bytes the page prints. A page byte that no longer
// matches is a page that has drifted, and an encoder byte that no longer matches the pin
// is a disagreement between the document and the implementation.
//
// conformance/message.txt carries the same message as a vector, so every implementation in
// the family runs it.

func goldenMessageSteps() []*step {
	n := big.NewInt
	bits := func(width int64, value *big.Int) *step {
		return &step{kind: stepBits, width: width, bits: value}
	}
	ranged := func(lo, hi, value *big.Int) *step {
		return &step{kind: stepInt, lo: lo, hi: hi, number: value}
	}
	fixed := func(fractionBits uint, lo, hi, raw *big.Int) *step {
		return &step{kind: stepFixed, fractionBits: fractionBits, lo: lo, hi: hi, number: raw}
	}
	pow2 := func(e uint) *big.Int { return new(big.Int).Lsh(n(1), e) }
	neg := func(v *big.Int) *big.Int { return new(big.Int).Neg(v) }

	q16_16 := n(1234*65536 + 32768)
	q48_16 := neg(n(54321*65536 + 12345))
	q16_16u := n(29999*65536 + 65535)
	q112_16 := neg(new(big.Int).Add(new(big.Int).Mul(n(98765432109), n(65536)), n(4321)))
	q64_64 := new(big.Int).Add(new(big.Int).Lsh(mustHex("0123456789ABCDEF"), 64), mustHex("0FEDCBA987654321"))

	return []*step{
		bits(4, n(13)),
		bits(11, n(1445)),
		bits(24, n(11259375)),
		bits(32, mustHex("DEADBEEF")),
		ranged(n(-100), n(100), n(-37)),
		ranged(neg(pow2(31)), new(big.Int).Sub(pow2(31), n(1)), n(-123456789)),
		{kind: stepBool, flag: true},
		{kind: stepFloat, bits: n(int64(math.Float32bits(3.1415926)))},
		{kind: stepCompressedFloat, fmin: 0, fmax: 10, fres: 0.01,
			bits: n(int64(math.Float32bits(5.0)))},
		{kind: stepDouble, bits: new(big.Int).SetUint64(math.Float64bits(1.0 / 3.0))},
		bits(8, mustHex("7F")),
		bits(16, mustHex("1234")),
		bits(32, mustHex("12345678")),
		bits(64, mustHex("123456789ABCDEF0")),
		{kind: stepIntRelative, previous: 100, number: n(101)},
		{kind: stepIntRelative, previous: 100, number: n(2100)},
		{kind: stepAlign},
		{kind: stepBytes, width: 7, data: []byte{0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0x01}},
		{kind: stepString, width: 16, data: []byte("golden")},
		{kind: stepWString, width: 8, units: []uint16{0x043C, 0x0438, 0x0440}},
		{kind: stepAlign},
		fixed(8, n(-100), n(100), n(-832)),
		fixed(16, n(-2000), n(2000), q16_16),
		fixed(16, n(-100000), n(100000), q48_16),
		fixed(16, n(0), n(30000), q16_16u),
		{kind: stepAlign},
		fixed(16, neg(pow2(57)), pow2(57), q112_16),
		fixed(64, neg(pow2(63)), new(big.Int).Sub(pow2(63), n(1)), q64_64),
	}
}

func mustHex(text string) *big.Int {
	v, ok := new(big.Int).SetString(text, 16)
	if !ok {
		panic("not hexadecimal: " + text)
	}
	return v
}

// formatGoldenBytes renders the message the way STANDARD.md prints it, twelve bytes to a
// line under a four space indent. SERIALIZE_PRINT_GOLDEN emits it, which is how the page's
// block is written when the message changes.
func formatGoldenBytes(data []byte) string {
	var out strings.Builder
	for i, b := range data {
		switch {
		case i%12 == 0:
			if i > 0 {
				out.WriteString("\n")
			}
			out.WriteString("    ")
		default:
			out.WriteString(" ")
		}
		fmt.Fprintf(&out, "0x%02X", b)
	}
	return out.String()
}

// goldenBytesFromStandard reads the byte block STANDARD.md prints under the marker line,
// exactly as hexArray reads a byte array out of serialize.h. The page is a source of
// bytes here, not prose: if it drifts, this goes red.
var goldenPageMarker = "The whole message is 112 bytes:"

func goldenBytesFromStandard(path string) ([]byte, error) {
	text, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	lines := strings.Split(string(text), "\n")
	start := -1
	for i, line := range lines {
		if strings.Contains(line, goldenPageMarker) {
			start = i + 1
			break
		}
	}
	if start < 0 {
		return nil, fmt.Errorf("STANDARD.md does not carry the line %q, so the page's golden bytes cannot be found", goldenPageMarker)
	}
	pair := regexp.MustCompile(`0x([0-9A-Fa-f]{2})`)
	var out []byte
	seen := false
	for _, line := range lines[start:] {
		if strings.TrimSpace(line) == "" {
			if seen {
				break
			}
			continue
		}
		if !strings.HasPrefix(line, "    ") {
			break
		}
		matches := pair.FindAllStringSubmatch(line, -1)
		if len(matches) == 0 {
			break
		}
		seen = true
		for _, m := range matches {
			out = append(out, byte(mustHex(m[1]).Int64()))
		}
	}
	if !seen {
		return nil, fmt.Errorf("STANDARD.md carries the marker line but no byte block under it")
	}
	return out, nil
}

func checkGoldenMessage(c *checker, standardPath string, pinned []byte) {
	steps := goldenMessageSteps()

	w := &BitWriter{}
	for _, s := range steps {
		if err := encodeStep(w, s); err != nil {
			c.err("golden message encode", err)
			return
		}
	}
	consumed := w.i
	encoded := w.flush()

	c.eq("golden message: the document's own encoder reproduces the C++ writer's 112 bytes",
		fmt.Sprintf("%X", encoded), fmt.Sprintf("%X", pinned))

	if os.Getenv("SERIALIZE_PRINT_GOLDEN") != "" {
		parts := make([]string, 0, len(steps))
		for _, st := range steps {
			switch st.kind {
			case stepAlign:
				parts = append(parts, "-")
			case stepFloat, stepDouble, stepCompressedFloat, stepBits:
				parts = append(parts, fmt.Sprintf("0x%X", st.bits))
			case stepInt, stepInt64, stepInt128, stepIntRelative, stepFixed:
				parts = append(parts, st.number.String())
			default:
				parts = append(parts, renderStep(st))
			}
		}
		fmt.Printf("expect value = %s\n", strings.Join(parts, " | "))
		fmt.Printf("golden message, %d bytes, %d bits consumed, measure floor %d:\n%s\n",
			len(encoded), consumed, worstCaseBits(steps), formatGoldenBytes(encoded))
	}

	page, err := goldenBytesFromStandard(standardPath)
	if c.err("golden message: STANDARD.md byte block", err) {
		return
	}
	c.eq("golden message: STANDARD.md prints the bytes the encoder produces",
		fmt.Sprintf("%X", page), fmt.Sprintf("%X", encoded))

}
