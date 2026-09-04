How to build serialize
======================

serialize builds with [CMake](https://cmake.org) (3.16 or newer) on Windows, MacOS and Linux.

## Building

Go to the command line under the serialize directory and enter:

    cmake -B build
    cmake --build build --config Release

Then run the tests:

    ctest --test-dir build --build-config Release --output-on-failure

Or run the binaries directly:

    ./build/bin/test
    ./build/bin/example

On Windows the binaries are under `build\bin\Release`, and you can open the generated `build\serialize.sln` in Visual Studio if you prefer to work there.

## Installing with Homebrew

serialize is in `homebrew/core`, so no tap is needed:

```
brew install serialize
```

The formula installs `serialize.h` and the CMake package config, so both `#include <serialize.h>` and `find_package(serialize CONFIG)` work out of the box. The `consumer` CI job builds a fresh CMake project outside this repository against the installed formula, on every push and every night, and fails while homebrew/core is behind this repository, naming both versions. At the time of writing it is failing: the formula serves 1.16.0 and this repository is at 1.16.2.

## Using serialize in your project

serialize is a single header. The simplest thing is to copy `serialize.h` into your project.

If you use CMake, you can consume it as a target instead — via FetchContent:

    include(FetchContent)
    FetchContent_Declare(serialize GIT_REPOSITORY https://github.com/mas-bandwidth/serialize.git GIT_TAG v1.16.2)
    FetchContent_MakeAvailable(serialize)
    target_link_libraries(your_target PRIVATE serialize::serialize)

or via `add_subdirectory`, or install it (`cmake --install build`) and use `find_package(serialize CONFIG REQUIRED)`. In all cases the `serialize::serialize` target carries only the include path: none of this repo's warning or fast-math flags leak into your build, and the test and example targets are only built when serialize is the top level project.

`GIT_TAG` names a release, never a branch. A release carries a stated format version, and both endpoints of a connection must run releases carrying the same format version or they do not interoperate. The format version is stated at the head of [STANDARD.md](STANDARD.md) and is not the library version; [COMPATIBILITY.md](COMPATIBILITY.md) lists the release of every implementation in the family that carries the current one. A floating branch moves one endpoint on its own schedule, which is how two endpoints end up on different format versions.

The library version is available as `SERIALIZE_VERSION` (and `SERIALIZE_VERSION_MAJOR/MINOR/PATCH`) after including the header.

## Floating point flags

The compressed float wire format requires exact `float32` rounding (STANDARD.md). The header pins the load-bearing roundings in-source with an optimization barrier, so the wire bytes are identical under every `-ffp-contract` setting — including GCC's default `-ffp-contract=fast` on FMA targets such as arm64 — and the test suite runs and passes at `off`, `on` and `fast` (CI builds all three). Two rules remain:

- `-ffast-math` (and `-Ofast`) are not supported: they license reciprocal approximation and reassociation, which change the wire in ways no barrier can pin. The test suite refuses such a build by name.
- Building with `-ffp-contract=off` is still the standing policy for the network libraries this header ships in (serialize, netcode, reliable, yojimbo, flow, rocketnet) — belt and braces on top of the in-source barrier, and the certification setting for golden vectors.

## Debug builds

    cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
    cmake --build build-debug --config Debug
    ctest --test-dir build-debug --build-config Debug --output-on-failure

Debug builds define `SERIALIZE_DEBUG`, which enables asserts and extra bounds checking. Release builds define `SERIALIZE_RELEASE`.

Benchmarking for the serialize family lives in [mas-bandwidth/schema](https://github.com/mas-bandwidth/schema)'s data-driven bench, which measures the generated codecs across every language on one corpus.

## Fuzzing

A libFuzzer harness lives in `fuzz.cpp`. Each input is run two ways: as hostile bytes fed to every `ReadStream` primitive, and as a source of values for a write→read round trip that traps on any mismatch. It needs clang (Apple clang doesn't ship the libFuzzer runtime, so use Linux or Homebrew LLVM on MacOS):

    cmake -B build-fuzz -DCMAKE_BUILD_TYPE=Debug -DSERIALIZE_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
    cmake --build build-fuzz
    ./build-fuzz/bin/fuzz

Build it in Debug so asserts stay enabled: reads from a `ReadStream` must fail by returning false, never by tripping an assert, and the fuzzer treats an assert as a crash. CI runs this harness for 60 seconds on every push, and for an hour nightly with a corpus that accumulates across runs. If a nightly run finds a crash, the reproducer input is uploaded as a workflow artifact.

If you have questions please create an issue at https://github.com/mas-bandwidth/serialize and I'll do my best to help you out.

cheers

 - Glenn
