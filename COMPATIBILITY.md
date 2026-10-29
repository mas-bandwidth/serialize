# Family compatibility

**Format version 2.0.** Two endpoints interoperate when they run releases
carrying the same format version. The format version names the wire, not a
library release, and it is stated at the head of [STANDARD.md](STANDARD.md).

These are the releases that carry format version 2.0:

| runtime | language | release |
|---|---|---|
| [serialize](https://github.com/mas-bandwidth/serialize) | C++ | `v2.1.3` |
| [serialize.c](https://github.com/mas-bandwidth/serialize.c) | C | `v2.1.1` |
| [serialize.cs](https://github.com/mas-bandwidth/serialize.cs) | C# | `v2.1.1` |
| [serialize.go](https://github.com/mas-bandwidth/serialize.go) | Go | `v2.1.1` |
| [serialize.rs](https://github.com/mas-bandwidth/serialize.rs) | Rust | `v5.0.0` |
| [serialize.js](https://github.com/mas-bandwidth/serialize.js) | JavaScript | `v2.1.2` |
| [serialize.dart](https://github.com/mas-bandwidth/serialize.dart) | Dart | `v2.1.1` |
| [serialize.java](https://github.com/mas-bandwidth/serialize.java) | Java | `v2.1.2` |
| [serialize.elixir](https://github.com/mas-bandwidth/serialize.elixir) | Elixir | `v2.1.1` |
| [serialize.lua](https://github.com/mas-bandwidth/serialize.lua) | Lua | `v2.1.2` |
| [serialize.luau](https://github.com/mas-bandwidth/serialize.luau) | Luau | `v2.1.3` |
| [serialize.moo](https://github.com/mas-bandwidth/serialize.moo) | moo | `v2.1.2` |
| [serialize.nut](https://github.com/mas-bandwidth/serialize.nut) | Squirrel | `v2.1.2` |
| [serialize.lobster](https://github.com/mas-bandwidth/serialize.lobster) | Lobster | `v2.1.2` |
| [serialize.goose](https://github.com/mas-bandwidth/serialize.goose) | Goose | `v2.1.2` |

The C++ implementation is the reference, and it carries 2.0 first. Every port
in the family re-vendors this standard and corpus and joins this table with a
release of its own; until it does, it carries format version 1.1, below, and a
1.1 endpoint and a 2.0 endpoint do not interoperate.

## What the pin is

Every release above vendors the standard and the corpus from one commit of this
repository:

    standard_commit  bd3d67e23c8acc2c45eff69633dbe16c029a2cba
    corpus_commit    bd3d67e23c8acc2c45eff69633dbe16c029a2cba

The pin names a commit and never a branch. `main` moves, so a claim proved
against `main` is a claim about whatever `main` happened to be that morning. The
pin moves when the family re-syncs, which is a deliberate act with a release
behind it.

## What 2.0 changed, and what it breaks

The principle *(Glenn Fiedler, 2026-10-22)*: the wire carries only integers and
bytes, and every value has exactly one encoding. **Every finite value 1.1 could
carry keeps its exact bytes.** What breaks:

* **`wstring` is removed.** A 1.1 stream with a `wstring` field has no 2.0
  reading, and code that calls `serialize_wstring`, `read_wstring` or
  `write_wstring` no longer compiles. Text is `string`, UTF-8. A C++ program that
  holds `wchar_t`, UTF-16 or UTF-32 text converts it with
  [serialize_utf.h](serialize_utf.h), shipped beside the library and outside the
  format, which refuses a lone surrogate exactly as the wire refuses an encoded
  one.
* **NaN and the infinities are refused** in `float` and `double`, on read, as
  `non_finite_float`, and asserted on write. A 1.1 stream that carried one is
  refused. Negative zero and the subnormals keep their exact bits.
* **`compressed_float` refuses a non-finite decode**, which only a
  non-conforming declaration can produce: one whose top, `min + delta`, rounds
  to an infinity. Such a declaration is now non-conforming as a whole, and its
  writer asserts in checked builds even for values that decode finite.
* **`int_relative` refuses the absolute tier carrying a bounded difference** as
  `non_canonical`. No conforming 1.1 writer emitted it; a 1.1 reader accepted it.
* **`serialize_end` is new**, and a packet reader is required to call it after
  its top-level object. A reader that calls it refuses trailing bytes
  (`trailing_data`) and a set padding bit in the final byte (`nonzero_padding`).
  A reader handed a buffer larger than the packet now refuses it at the end.
* **Every refusal has a reason** from a closed list, `ReadStream::GetRefusal`
  reports it, and every refused vector in the corpus states it. A port that
  refuses for the wrong reason fails the corpus.

The library's own version moved to 2.0.0 for the same reason, so a CMake
consumer asking for `find_package(serialize 1 CONFIG)` does not pick it up.

## Format version 1.1

Format version 1.1 is the wire of the releases below, all pinned to commit
`39cc8543bb502b83cff5046a2f865ab1ab530cb7` of this repository. It is superseded,
and listed so an endpoint on it can find its peers.

| runtime | language | release |
|---|---|---|
| serialize | C++ | v1.17.0 |
| serialize.c | C | v1.9.2 |
| serialize.cs | C# | v1.9.1 |
| serialize.go | Go | v1.15.1 |
| serialize.rs | Rust | v2.4.0 |
| serialize.js | JavaScript | v1.4.2 |
| serialize.dart | Dart | v1.1.2 |
| serialize.java | Java | v1.1.2 |
| serialize.elixir | Elixir | v1.1.2 |

## How this page is proved

[COMPATIBILITY.txt](COMPATIBILITY.txt) holds the format 2.0 rows machine
readably, and the `family matrix` job checks every one of them: it checks out
each repository at the row's release, diffs the `STANDARD.md` and `conformance/`
that release vendors against the pinned commit, and checks that the release's
version file carries the release's version. It runs on every push to `main` and
nightly, so a port that drifts after release is caught rather than assumed. A
row that cannot be proved makes the job red.
