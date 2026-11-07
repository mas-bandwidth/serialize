# Wide-declaration bench record

Paired before/after packet-read numbers for the 128 bit declaration-width
change in `serialize.h` (the `bound_fits_int64`, `value_in_int128_range`,
`bound_fits_int128` and `value_in_int_relative_domain` checks on the
`serialize_int64` / `serialize_int` / `read_int64` / `read_int` /
relative-read paths), which the change shipped without fleet numbers.
This record supplies them after the fact; the verdict below is from
these runs.

## Setup

- Host: fleet `amd-2` (hostname `b2`), Linux x86_64, `taskset -c 2`
- Base: `8d6064fd5730bdcf4d80297edfaeb447dcb60f59` (parent of the change)
- Head: `db5a7435a67e90b5f95be27325524a2ba3a52bef` ("Judge 128 bit ranged
  declarations in their own width")
- Bench: scratch loop (no in-repo bench target exists), 42-byte packet,
  `noinline` read of `serialize_int64` in [-1000000, 1000000],
  `serialize_int` in [-1000, 1000], `SerializeBits(17)` and
  `SerializeBytes(32)` through a fresh `ReadStream` per packet;
  50,000,000 iterations per run; 9 runs per side, base and head
  alternating on the same pinned core; `-O3 -DNDEBUG -std=c++11`
- Compilers: GCC 15.2.0 and Clang 21.1.8 (fleet builds)

## Results (Mmsg/s, 9 runs each)

| Compiler | Base median | Head median | Head/base | Base range | Head range |
|---|---|---|---|---|---|
| GCC | 445.128 | 446.684 | 1.003 | 436.938–447.974 | 442.970–447.323 |
| Clang | 447.384 | 445.713 | 0.996 | 444.002–490.665 | 444.296–446.825 |

The two high Clang base runs (486.403, 490.665) are shared-host noise;
the remaining seven base runs sit in 444.002–447.732.

## Verdict

Within noise: neither compiler is measurably slower at head. GCC is
+0.3% and Clang −0.4% at the median, with fully overlapping ranges.
This agrees with the coordinator's independent amd-2 measurement of
the same pair (base median 264.909 vs head 265.239 Mmsg/s, ratio
1.001, ranges overlapping, on a heavier packet loop).
