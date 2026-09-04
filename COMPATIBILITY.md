# Family compatibility

**Format version 1.1.** Two endpoints interoperate when they run releases
carrying the same format version. The format version names the wire, not a
library release, and it is stated at the head of
[STANDARD.md](STANDARD.md).

These are the releases that carry format version 1.1:

| runtime | language | release |
|---|---|---|
| [serialize](https://github.com/mas-bandwidth/serialize) | C++ | `v1.16.2` |
| [serialize.c](https://github.com/mas-bandwidth/serialize.c) | C | `v1.9.2` |
| [serialize.cs](https://github.com/mas-bandwidth/serialize.cs) | C# | `v1.9.1` |
| [serialize.go](https://github.com/mas-bandwidth/serialize.go) | Go | `v1.15.1` |
| [serialize.rs](https://github.com/mas-bandwidth/serialize.rs) | Rust | `v2.3.2` |
| [serialize.js](https://github.com/mas-bandwidth/serialize.js) | JavaScript | `v1.4.2` |
| [serialize.dart](https://github.com/mas-bandwidth/serialize.dart) | Dart | `v1.1.2` |
| [serialize.java](https://github.com/mas-bandwidth/serialize.java) | Java | `v1.1.2` |
| [serialize.elixir](https://github.com/mas-bandwidth/serialize.elixir) | Elixir | `v1.1.2` |

The library versions differ across the family and always will: they count each
implementation's own releases. The format version is the only number that says
whether two endpoints can talk to each other.

## What the pin is

Every release above vendors the standard and the corpus from one commit of this
repository:

    standard_commit  7e0515e952f3373d001ec1899adf9dffb823e1c5
    corpus_commit    7e0515e952f3373d001ec1899adf9dffb823e1c5

The pin names a commit and never a branch. `main` moves, so a claim proved
against `main` is a claim about whatever `main` happened to be that morning. The
pin moves when the family re-syncs, which is a deliberate act with a release
behind it.

## What 1.1 changed, and why the number moved

The bytes are unchanged for every value that is legal under 1.1. A writer that
stays inside the format produces the same stream it produced before, and a
reader decodes it to the same value.

What changed is refusal behavior, and refusal behavior is part of the format.
A reader that accepts a stream the standard says must be refused is not
compatible with one that refuses it: the two disagree about which streams exist,
which is a disagreement about the wire even when every legal stream round trips
identically. That is why the version moved rather than staying at 1.0 with a
note. [STANDARD.md](STANDARD.md) lists the rulings.

## How this page is proved

[COMPATIBILITY.txt](COMPATIBILITY.txt) holds these rows machine readably, and
the `family matrix` job checks every one of them: it checks out each repository
at the row's release, diffs the `STANDARD.md` and `conformance/` that release
vendors against the pinned commit, and checks that the release's version file
carries the release's version. It runs on every push to `main` and nightly, so a
port that drifts after release is caught rather than assumed. A row that cannot
be proved makes the job red.
