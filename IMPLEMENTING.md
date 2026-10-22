# Implementing serialize

[STANDARD.md](STANDARD.md) states the format, and every rule in it cites the
conformance vectors that prove it. This page is the practice the family follows
when it builds an implementation of that format: how to build one, not what the
bytes are. No stream can observe any of it, so none of it is a conformance rule,
and an implementation is judged on it by review, not by the corpus.

Until format 2.0 most of this lived in STANDARD.md, as "Implementation Law" and
beside the operations. It moved here on 2026-10-22, when STANDARD.md began
requiring every rule it states to cite a vector.

## The job

*(Adopted 2026-08-16, after a six-implementation audit found invented contracts
replicating port to port.)*

When this library is ported to a language, the job is to find **the fastest
correct implementation in that language.** Correct is defined by STANDARD.md and
its corpus; fastest is defined by measurement.

**Sources.** An implementation derives from STANDARD.md and the corpus. Sibling
ports are never sources: copying a sibling's behaviour because it is the nearest
working example is how inventions travel disguised as specification. If the
standard lacks what is needed to implement correctly and fast, the standard is
too loose: tighten it upstream, with a vector, and never improvise in a port.

**Speed is part of the job.** Among correct implementations of an operation,
the fastest correct option is the one to ship. A new approach a port invents is
welcome provided it is the fastest correct option, and the C++ implementation
should then adopt it. A port slower than the C++ implementation for any reason
other than a documented language necessity is defective, and the deviation and
its necessity are documented where the divergence lives: C and C++ at parity,
systems languages within a few percent with every residual attributed to a named
language mechanism.

## The check model

The caller is responsible for well-formed writes. Where the language has checked
builds (assertions enabled), write-side contract validation uses them and
nothing else: release builds perform **zero** write-side validation in C and
C++, and the minimum the language permits elsewhere. Readers perform exactly the
refusal rules of STANDARD.md, each reporting its reason, and nothing more. A
check that neither the standard states nor the language forces is an invented
contract, whatever its justification sounds like.

What a writer's checked build asserts, in every implementation that has one:

* a ranged value outside `[min, max]`, and `min > max`;
* a `bits` value that does not fit its width;
* a `float` or `double` that is NaN or an infinity, tested on the bits after any
  narrowing the operation performs (a finite double above `FLT_MAX` narrows to
  an infinity, and that is what would be sent);
* a `compressed_float` value that is not finite, and a declaration whose
  `delta`, `delta / res`, or top decode `min + delta` is not finite in `float32`;
  the top decode check belongs on the write path only, because a reader with
  such a declaration refuses its non-finite decode at runtime;
* a `string` that is not well-formed UTF-8, or does not fit `buffer_size - 1`;
* an `int_relative` whose `previous` or `current` is outside the domain, or
  whose `current` is not above `previous`.

A write-side assertion inspects the caller's value as the caller passed it,
before any narrowing the operation performs on the way to the wire. An assertion
placed after a narrowing sees a value already truncated to the operation's
width, so it cannot report the input it exists to diagnose.

## The buffer contract

Reading whole words through the end of the buffer, with the allocation extended
so the final word load is legal, is the accepted best practice: the C++ and C
implementations load 64-bit windows at byte granularity and require their caller
to allocate at least 8 bytes beyond the data. Bytes past the end may be loaded
and are never interpreted: no decoded value and no accept or refuse decision
depends on them. Every implementation states which allocation contract its
caller is under. Machinery that avoids the slack at the cost of per-operation
work in the hot path is a slower correct option, and is refused by the speed
rule. A copying entry point that takes an exact-length input and copies it into
a slack-backed buffer prices the copy once at the boundary, and is fine.

A reader's `end` is what makes the length exact: the stream handed to a reader
is the packet's bytes, the count a writer reported, and the slack is behind it.

## Failure

The recommended latch is to poison the stream's position one bit past the end of
the buffer, so the past-end check every read already performs refuses every
later read, and to record the first refusal's reason only on the refusal path:
the latch costs the read path nothing. A runtime whose idiom is to throw may
throw; the reason travels with the exception.

## Fixed point conversions

The wire never rounds a fixed point value: the round trip is exact. Wherever a
runtime converts a value into a Q format or narrows one out of it, it rounds
ties **half away from zero**: `( raw + half ) >> drop` for `raw >= 0`, and
`-( ( -raw + half ) >> drop )` for `raw < 0`, with `half = 1 << ( drop - 1 )`.
The naive arithmetic shift floors, so applying `( raw + half ) >> drop` to a
negative raw rounds ties toward +infinity and diverges by one raw step on exact
negative ties. Every implementation that exposes such a conversion tests a
negative tie.

## Text that is not UTF-8

serialize 2.0 carries text as UTF-8 only. A C++ program that holds text as
`wchar_t`, UTF-16 or UTF-32 converts at its own boundary with
[serialize_utf.h](serialize_utf.h), which refuses a lone surrogate exactly as
the wire refuses an encoded one. A port whose native strings are UTF-16 (Java,
C#, JavaScript, Dart) converts with its runtime's strict codec: strict, because
the lenient ones replace a lone surrogate with `U+FFFD`, which sends a different
string than the caller holds. Neither the helper nor a port's codec is part of
the format.

## The C++ read and write forms

The C++ implementation also spells every operation as a `read_` and a `write_`
macro, for code paths that only ever read or only ever write, and they produce
the bytes of their `serialize_` counterpart. They are C++ API, not format, and
other implementations do not carry them: one stream abstraction with a read, a
write and a measure conformer carries every operation.
