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
C++, and the minimum the language permits elsewhere. An implementation with no
unchecked build checks in every run, and traps on misuse (U9). Readers perform exactly the
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
the latch costs the read path nothing. A refusal is a value in every
implementation, never a thrown exception (U7): `false`, a result, or an error
tuple, carrying its reason, whatever the runtime's idiom elsewhere.

The latch has three properties, and every implementation keeps all three: the
stream is refused exactly when a reason is recorded, the reason never changes
once recorded, and no read succeeds after a refusal. They are modelled in
[tla/ReadStreamLatch.tla](tla/ReadStreamLatch.tla), checked with TLC.

## The caller's refusal (U2)

*(Format 2.0, standard 2.1, 2026-10-27.)* STANDARD.md's `rejected` reason is the
caller's own refusal of a value whose bytes were well formed, and every
implementation gives its caller a way to make one. How it is spelled follows the
implementation's idiom for everything else it does on a stream:

* **The reject entry point.** The C++ reference spells it as a macro beside the
  others, `serialize_reject( stream )`, which fails the stream through the same
  path every other refusal takes (`serialize_fail( stream, REFUSAL_REJECTED )`)
  and returns false from the caller's serialize function. A port with methods
  spells it `stream.reject()` or its language's equivalent; a port whose
  read returns no successor stream returns the refused result. Either way the
  refusal is a value, never a thrown exception (U7). It must route
  through the same latch as a wire refusal, so the first reason wins without a
  special case.
* **A nested object that fails.** Most callers refuse by returning failure from
  a nested object's serialize function, and the object operation must latch that
  as `rejected` when nothing inside it refused. Done through the same latch, the
  first reason is kept for free.
* **Reject is read only.** On a write or a measure there is nothing to refuse:
  calling it there is caller error, asserted in checked builds like any other
  write-side misuse, and it validates nothing at runtime in a release build.
* **A refused stream never reports none.** An accessor for the reason returns
  `rejected`, or the wire's reason, on every refused stream. An implementation
  that represents "no refusal" and "refused for no reason" by the same value has
  lost the distinction the reason exists to carry.

The corpus spells the caller's refusal as the `reject` step: through the reject
entry point at the top level, and as the object's serialize function failing
inside an `object`. A runner drives both.

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
macro, except `compressed_float`, which has no `write_` form (and no `read_`
form), for code paths that only ever read or only ever write, and they produce
the bytes of their `serialize_` counterpart (test_write_forms_wire_identical
compares each `write_` macro with its twin from every start offset). They are API,
not format: one stream abstraction with a read, a write and a measure conformer
carries every operation, and an implementation need not carry split forms. Split
`read_`/`write_` forms are an allowed extra for C and C++; any implementation may
carry split `write_` forms and typed conveniences under the conditions U15 sets.
The fixed-width aliases `uint8` to `uint64` are not extras: STANDARD.md defines
them as `bits` at their width, and they are part of every implementation's
surface.

## Uniform behaviour: U1 to U16

*(Rowan's rulings from the cross-port audit, 2026-10-27, on Glenn's requirement
that behaviour be uniform across every port and in the spec.)* None of these can
be a corpus vector: each is write-side, measure-side, API-shape or caller-misuse
behaviour, which no stream observes. They are nonetheless normative: **every
implementation must** keep each one, proves it with its own unit tests, and
cites the rule by its number. A port that cannot keep one names the gap and the
release that closes it, exactly as for a STANDARD.md rule.

**U1. Write buffers are a multiple of 8 bytes.** Every implementation must
require it and check it wherever it checks write misuse. Its documentation says
8, never 4. *(C++: `BitWriter` asserts `bytes % 8 == 0`.)*

**U2. A caller's refusal latches `rejected`.** Every implementation must expose a
public reject and a public object composition, and both must latch `rejected`
when nothing refused first. It is terminal, and the first reason wins. See "The
caller's refusal" above. *(C++: `serialize_reject`, `serialize_object`;
`test_serialize_reject`, the `reject` vectors, the `object-latch-rejected` and
`reject-reason` controls.)*

**U3. A writer never sends bytes that differ from the caller's value.** String
encoding must use the strict UTF-8 codec: a lone surrogate or any other
unencodable text is write misuse, never silently replaced (no `?`, no `U+FFFD`).
A measure stream follows the same rule. *(Ruling, 2026-10-27: it is write misuse
like any other, so U9 decides what it does. A checked build traps on it, which is
the guarantee: no checked build ever replaces it. An unchecked build may compile
the check out, and its bytes after the misuse are unspecified, as they are for
every other write misuse; a lenient codec's `?` or `U+FFFD` there is that
unspecified output, never a documented behaviour, and no build may replace on
purpose with the check on. The C++ reference in release writes invalid bytes as
given, and is unspecified in the same way.)* *(C++: text is already UTF-8 bytes
and is written as given; the validity assertion below covers it.)*

**U4. Every implementation must check UTF-8 and NUL on string write**, wherever
it checks any write misuse, and at least in checked builds. *(C++: the write path
asserts `serialize_string_is_valid_utf8`; the length is `strlen`, so an interior
NUL cannot reach the wire from C++ by construction.)*

**U5. `int` bounds are int32.** A `min` or `max` outside int32 is caller error and
must be caught on the caller's original value, before anything narrows it. Never
a silent zero-bit or narrowed field. On read it is a bad declaration, refused as
`value_out_of_range` (U7, U16). *(C++ 2.1: `serialize_int`, `read_int` and
`write_int` assert `serialize::int_bounds_are_int32` on write and measure and
refuse on read; asserts.cpp and test_uniform_rules prove it.)*

**U6. A measure stream refuses nothing** (STANDARD.md, "The Measure Stream").
A measure makes every check a write makes, except the room left in a buffer it
does not have, and catches misuse by the same trap (U9). It never latches
anything: no misuse, `fail`, `reject` or failing nested object sets a reason or
makes a later measure fail. *(C++: the operation macros assert on
`Stream::IsWriting`, which a `MeasureStream` sets, so a measure asserts exactly
what a write asserts, except room and buffer size, which a measure does not
have; `serialize_fail` on a measure returns false and records
nothing; test_uniform_rules.)*

**U7. A reader never traps, and a refusal is a value.** Every refusal on a read
stream, from the wire, from the caller, or from caller misuse, must come back as a value: `false`,
a result, or an error tuple carrying its reason. It is never a thrown exception,
in any implementation, whatever the runtime's idiom. Caller misuse on read, such
as a negative byte count or a string buffer size below 1, must refuse as
`value_out_of_range`, and never raise, abort, assert or throw. The failure state
is checked first, so a stream that already failed keeps its first reason. *(C++ 2.1, from the
2026-10-27 reference audit: a string buffer below 1 on read reached the length
field's `min <= max` assertion in a checked build and, in release, a length range
of `[0, -1]` that let the wire choose a payload copied past the caller's buffer;
it now refuses. A read stream over a negative byte count refused but reported
none; it now latches `value_out_of_range` at initialization. A ranged read with
`min > max` accepted wrong data; it now refuses. test_uniform_rules.)*

**U8. Over-wide values on write are caught in the checked build.** Every
implementation's checked build (debug, dev, `-ea`, or an always-checked port)
must catch a value that does not fit its bit width, as "The check model" above
says; masking is not checking. Release output after write misuse is
unspecified, and no mask is required on the fast path. *(C++: `serialize_bits`
asserts `value_fits_in_bits` on the caller's value before narrowing; the ranged
macros assert `value_in_int64_range` / `value_in_int128_range` on the caller's
value as passed, sign included, so an unsigned `2^64 - 1` is not mistaken for
`-1`; asserts.cpp proves both.)*

**U9. One write-misuse mechanism: the trap.** *(Ruling, 2026-10-27, after a
survey of every implementation found three mechanisms in use: an assertion, a
latched reason, and a split that latched bad values but trapped on bad
declarations. The C++ reference defines the one below, so it is the family's.)*
Write misuse is everything the check model, U1, U3 to U5, U8, U10 and U16 name on
a write or a measure stream: a value outside its range or its bit width; a
`float`, `double` or `compressed_float` value that is not finite; a bad
declaration; text that is not well-formed UTF-8, holds a NUL, or does not fit
`buffer_size - 1`; a write buffer that is not a multiple of 8 bytes; and an
operation that does not fit the room left in the buffer. Every implementation
catches every one of them the same way, on every operation, `compressed_float`
included. Write misuse is not a refusal: U7's rule that a refusal is a value
governs refusals, and a trap is not one.

* **A checked build traps at the call**, before the call returns, and before
  any bit of the operation is written wherever U10 makes room a whole-operation
  check. The trap is the language's assertion, or the nearest thing to it
  that does not come back to the caller as a value: `assert` in C and C++, a
  panic in Go and Rust, an `AssertionError` under `-ea` in Java,
  `Debug.Assert` in C#, `assert` in Dart, a thrown error in JavaScript,
  Squirrel and Luau, `error` in Lua and moo, `raise` in Elixir, an abort in
  goose and lobster. Each implementation's documentation names its trap.
* **An implementation with no unchecked build is a checked build, always**, and
  traps in every run: Go, Lua, Luau, Squirrel, Elixir, goose, lobster and moo
  today.
* **Write misuse never latches.** A write or measure stream has no refusal state
  for misuse to set: no misuse records a reason or makes a later operation
  fail, and none returns false except `reject` in an unchecked build, below. `value_out_of_range`, `non_finite_float`, `invalid_utf8`,
  `embedded_nul` and `past_end` are a reader's reasons; latching one on a write
  is a second mechanism, and is not allowed.
* **An unchecked build** (release, `NDEBUG`, no `-ea`, production) may compile
  every check out, and its output after misuse is unspecified (U8). Nothing
  about it is promised beyond `reject`'s result below, and a caller must never
  depend on it.
* **`fail` on a write or a measure returns false, records nothing, and never
  traps, in every build:** it is the path every refusal takes, so a serialize
  function shared by the read and write sides can call it. A nested object
  whose serialize function returns false on a write or a measure makes the
  object operation return false, and records nothing.
* **`reject` on a write or a measure is caller misuse with one promised
  unchecked result.** It traps in a checked build, as write misuse does, so an
  always-checked implementation raises on it in every run, which is what the
  C++ reference does in a debug build. In an unchecked build it returns false
  and records nothing, the one unchecked result U9 promises, because it is
  spelled on the path every refusal takes.
* **`compressed_float`'s clamp is no exception.** A value that is NaN or an
  infinity is write misuse and traps in a checked build, never clamped into
  range there. The clamp that keeps C++'s release build defined is unchecked
  build behaviour, unspecified to callers. Schema's packet floats are finite
  only, so a conforming writer never has one to send.

The C write error flag (U15) is the one allowed record on a write stream:
`serialize_write_fail` sets it, in every build, for the caller to read. Misuse
never sets it, it makes no operation return false or trap, and a later write
emits exactly what it would have without it. *(C++: `serialize_assert` everywhere a write or
a measure checks, compiled out in release, after which every WriteStream and
MeasureStream operation returns true; `serialize_fail` there returns false
without asserting; `serialize_reject` there asserts, and returns false;
`serialize_compressed_float` asserts a non-finite value and only release
clamps it. asserts.cpp and test_compressed_float_non_finite_asserts prove them.)*

**U10. A write checks room for the whole operation before writing any of it.** No
length bits or align bits may be written ahead of a payload that does not fit.
*(C++ 2.1: `bytes` and `string` assert the whole operation's room, field, align
and payload, before the first bit; asserts.cpp proves the whole-operation check
is the first assertion to fire.)*

**U11. A writer's buffer is unspecified until flush.** Flush is required, and
every implementation's documentation says so. *(C++: `WriteStream::Flush`.)*

**U12. `end` on a write or measure stream is a no-op returning true.** *(C++:
test_uniform_rules.)*

**U13. Re-initialising a stream is optional** (STANDARD.md, "Failure is
terminal": a stream is re-initialized or discarded). An implementation without
it says so in its documentation. *(C++: `ReadStream::Initialize` and
`InitializePadded` clear the latch.)*

**U14. Every implementation must expose the refusal-name function**, with the
family spellings and `"none"` for no refusal, and a public `fail( reason )`. Native
accessor names may differ (`GetRefusal`, `refusal()`, `err()`), but the names
they return must match. On a read stream, `fail` with no reason, or with any
value outside the closed list, must record `rejected`: no assertion, no trap,
and never none, because a refused stream that reported none would be relabelled
`past_end` by the next read. *(C++: `serialize::refusal_name`,
`serialize::serialize_fail`, `ReadStream::GetRefusal`; `ReadStream::Fail` maps
`REFUSAL_NONE` and any value at or past `REFUSAL_NUM_REASONS` to
`REFUSAL_REJECTED`, in every build, since 2.1; test_uniform_rules, and the
`OldFail` witnesses in tla/ReadStreamLatch.tla.)*

**U15. The allowed extras, and the language necessities.** An implementation may
carry these beyond the reference surface, and nothing else without a ruling:
`Bits64` forms, `Continue`/`Until` loop helpers, batch APIs, the C write error
flag, split `read_`/`write_` forms (C and C++), and the wire-identical write and
typed forms below.

*Split write forms and typed conveniences (ruling, 2026-10-27: serialize exists
to serve schema).* Any implementation may offer split `write_` forms, and typed
conveniences, that are each one reference operation at a fixed declaration. The
fixed-width aliases `uint8` to `uint64` are reference operations already
(STANDARD.md, `bits`), not conveniences. Each form is allowed only if all of the
following hold:

* **It is wire-identical to its unified form.** It writes the same bytes and the
  same bit count for every value its unified form accepts and for every
  declaration, from every start offset.
* **It makes every write-misuse check its unified form makes**, through the
  same mechanism: the check model, U3 to U5, U8 to U10 and U16. Release
  behaviour after misuse stays unspecified, as U8 says.
* **The implementation documents, next to each form, why the form exists.** A
  typical reason is that the unified call takes the value by reference so that
  the read side can store through it, which forces a writer to copy the value or
  bind it mutably.

A form that is not wire-identical is a different operation, which is a format
change and never an extra.

*Conformance note.* No corpus vector can see an API form, so each implementation
proves the conditions with its own unit tests:

* Every such form is written against its unified twin after a prefix of every
  length from 0 to 63 bits, so it starts at every position in a 64-bit word,
  alignment and word carry included. The bytes and the bit counts must match.
* The values cover each operation's edges: the 64-bit split, a degenerate range,
  the full range, and a float's exact bits.
* Each write misuse is shown to be caught by the form as its unified twin catches
  it.

The corpus runner drives the unified forms, which remain the operations the
corpus pins.

The language necessities: moo's
numbers are Q48.16, so its exact `*Bits` float forms are its primary API,
documented as such, and moo's runner must drive the `*Bits` forms. A missing
reference operation is a gap, never an extra: `serialize_copy_string` and
`compressed_float_precomputed` must exist everywhere they are missing today.

*`serialize_copy_string` (ruling, 2026-10-27).* It is a reference helper, not a
wire operation, and every implementation carries it with one result, defined on
the source's UTF-8 bytes: take the bytes before the first NUL; if there are more
than `dest_size - 1`, keep the first `dest_size - 1` of them, then drop bytes
from the end while the first byte left out is a continuation byte
(`10xxxxxx`). For well-formed UTF-8 that is the longest prefix that fits and does
not end inside a character, and `serialize_string` with
`buffer_size = dest_size` accepts it. `dest_size` below 1 is write misuse (U9).
In an implementation whose strings are UTF-16, `dest_size` counts the bytes of
the strict UTF-8 encoding, and a source the strict codec refuses is write misuse
(U3).

* Where the destination is a caller-owned fixed-capacity buffer (C, C++,
  goose), the helper writes that prefix and zeros the rest of the buffer, so it
  is NUL terminated.
* Where strings are values (every other implementation), the helper returns
  the prefix, or assigns it through the destination reference the language
  uses for a string read. Assigning a string value copies it but does not bound
  it, and bounding is the helper's job, so a value-string implementation
  carries it too.

*(C++ 2.1.1: the copy backs off to a whole character; before, it cut at a byte
and could hand `serialize_string` text its write assertion refuses.
test_copy_string.)*

*Exports beyond the reference operations (ruling, 2026-10-27).* An
implementation's public surface is the reference operations, the three streams,
the refusal list and its name function (U14), `serialize_copy_string`, and the
extras above. Beyond those it may export only:

* **The reference's own public helpers**, spelled in its idiom and behaving as
  the reference's do: `popcount`, `log2`, `bits_required`, `bits_required64`,
  `bits_required128`, `value_fits_in_bits`, `bswap`, `signed_to_unsigned`,
  `unsigned_to_signed`, `float_bits_are_finite`, `double_bits_are_finite`,
  `compressed_float_params`, `string_is_valid_utf8`, `BitWriter`,
  `BitReader`, the 128-bit integer types, and the accessors the reference
  streams carry (bits processed, available and remaining, the context, the
  refusal). None of them is required.
* **Language necessities**, each documented as one where it is exported: what
  the language lacks that the reference's types give C++ for free. A float32
  rounding function and float bit casts where the language has no float32
  type; 64-bit and 128-bit integer carriers, with the constructors and the
  arithmetic their operations need, where the language has no such integers;
  by-reference holders where a scalar cannot be passed by reference; an
  offset and a length on `bytes` (and on its split write form) where the
  language cannot point into the middle of a buffer, standing for the
  reference's pointer and count and wire-identical to the operation on that
  span (serialize.js's span form, which schema's generated code relies on); the
  language's error or result type for a refusal.

Anything else is internal. Where the language can hide a name, it is hidden;
where it cannot (lobster has no private names), the implementation documents
which names are internal and promises nothing about them.

**U16. Caller declarations are validated on every stream before any bits move.**
A `bits` count is an integer in its range; a fixed point `min` and `max` fit the
Q format; a ranged `min` does not exceed its `max`; a string `buffer_size` is at
least 1. `compressed_float` precomputed constants are checked for what can be
checked without the declaration they came from: `bits` is consistent with
`max_integer_value` (`bits_required( 0, max_integer_value )`),
`max_integer_value` lies in `[1, 4294967040]`, and `delta` is positive and
finite. Destination capacity applies only where the API receives a destination
with a capacity of its own, such as a fixed array or a limited builder: there a
read must refuse as `value_out_of_range` when the declared size exceeds that
capacity. A bad declaration is caller error by the implementation's
write-misuse mechanism on write and measure, and must refuse as
`value_out_of_range` on read (U7). Never wrong data, and never a trap the wire
chooses. *(C++ 2.1: `SerializeBits` and
`serialize_bits` refuse a bit count outside their range on read; the ranged
reads refuse `min > max`; `serialize_int64`, `read_int64`, `serialize_int128` and
`read_int128` refuse bounds that arrive as wider unsigned values and do not fit (an
unsigned `2^64 - 1` is not `-1`); `int_relative` refuses a `previous` outside its
domain, judged before narrowing; the precomputed `compressed_float` refuses on read
constants whose bit count is not `bits_required( 0, max_integer_value )`, a
`max_integer_value` outside `[1, 4294967040]`, or a `delta` that is not positive
and finite; the C++ API hands `bytes` and `string` a bare pointer with no
capacity of its own, so the capacity rule has nothing to check there; a
fixed point declaration that does not fit its Q format is a compile error;
string `buffer_size` is U7. On write and measure each of these stays a debug
assertion. test_uniform_rules proves the read refusals.)*
