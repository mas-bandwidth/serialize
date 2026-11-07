# ReadStream size-check bench record

Location: this repository had no `bench/` directory and no in-repo bench
target; prior packet numbers live in external schema-bench evidence
(`docs/roadmap.sexp`, speed-profile-cpp). This file starts the in-repo
convention at `bench/results/` for this change. The bench source was a
scratch loop over the touched primitive (no bench target exists), run
only on a fleet host, never on the studio.

## Setup

- Host: fleet `amd-2` (hostname `b2`), Linux x86_64, `taskset -c 2`
- Base: `8e3711023424dd5661ee20f0ce06968d456446dc`
- Head: branch `sprint/serialize-audit-cpp/serialize-cpp-stream-size-before-arithmetic`
  (size validation before `bytes * 8`, subtraction request checks,
  single unsigned compare in `byte_count_fits_reader`, lean reader
  constructors)
- Bench: 42-byte packet, `noinline` read of SerializeBits(17),
  SerializeInteger, SerializeInteger64, SerializeBytes(32) through a
  fresh `ReadStream` per packet; 50,000,000 iterations per run;
  9 alternating base/head pairs per compiler; `-O3 -DNDEBUG -std=c++11`
- Compilers: GCC 15.2.0 and Clang 21.1.8 (fleet builds)

An earlier form (constructors delegating to `Initialize`, two-compare
signed check) measured about 6% slower under Clang on the same host
(median ratio 0.936, 9 pairs). The lean-constructor form below was
re-measured in full before shipping.

## Results (Mmsg/s, 9 runs each)

| Compiler | Base median | Head median | Head/base | Base range | Head range |
|---|---|---|---|---|---|
| Clang | 133.646 | 136.380 | 1.020 | 121.348–142.965 | 121.122–141.616 |
| GCC | 482.468 | 623.717 | 1.293 | 381.551–663.134 | 382.835–666.248 |

## Verdict

Within noise: neither compiler is measurably slower at head. Clang is
+2.0% at the median with fully overlapping ranges; GCC's spread on this
shared host is wider than any median difference. The hot change ships
on these numbers.
