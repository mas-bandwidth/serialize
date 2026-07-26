# Security Policy

serialize is a bitpacking serialization library. Its read path consumes buffers that in
practice arrive from the network, so a malformed or hostile stream must not be able to
read or write out of bounds.

## Reporting a vulnerability

**Please do not report security issues in public GitHub issues or pull requests.**

Report privately through either channel:

- **GitHub private vulnerability reporting** (preferred): on this repository, go to the
  **Security** tab → **Report a vulnerability**. This opens a private advisory visible only
  to the maintainers.
- **Email**: glenn@mas-bandwidth.com.

Please include enough detail to reproduce: the affected component and version/commit, a
description of the flaw, and — where possible — a proof-of-concept input or a small patch.
Fuzzing crash artifacts (a crashing input file plus the target name) are ideal.

We will acknowledge your report, keep you updated on our assessment, and coordinate
disclosure timing with you. We prefer coordinated disclosure and will credit reporters who
wish to be named.

## Scope

In scope — bugs in the serialize library itself (`serialize.h`, and the sources under
this repository).

Especially of interest: memory-safety issues in the **read** path reachable from a
hostile buffer — out-of-bounds reads past the end of a stream, integer overflow in bit or
byte counts, and any way for a serialized length or array count to drive an allocation or
a copy without being bounds-checked against what remains in the buffer.

serialize performs no encryption and no authentication; it is a wire-format library. It is
normally used underneath a layer that authenticates (netcode). That does not put memory
safety out of scope — a stream is only trustworthy if the layer above actually verified it,
and we would rather serialize be safe on its own.

## Supported versions

Security fixes land on the latest release. We do not backport to older release lines.
