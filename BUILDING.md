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

## Debug builds

    cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
    cmake --build build-debug --config Debug
    ctest --test-dir build-debug --build-config Debug --output-on-failure

Debug builds define `SERIALIZE_DEBUG`, which enables asserts and extra bounds checking. Release builds define `SERIALIZE_RELEASE`.

## Fuzzing

A libFuzzer harness lives in `fuzz.cpp`. Each input is run two ways: as hostile bytes fed to every `ReadStream` primitive, and as a source of values for a write→read round trip that traps on any mismatch. It needs clang (Apple clang doesn't ship the libFuzzer runtime, so use Linux or Homebrew LLVM on MacOS):

    cmake -B build-fuzz -DCMAKE_BUILD_TYPE=Debug -DSERIALIZE_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
    cmake --build build-fuzz
    ./build-fuzz/bin/fuzz

Build it in Debug so asserts stay enabled: reads from a `ReadStream` must fail by returning false, never by tripping an assert, and the fuzzer treats an assert as a crash. CI runs this harness for 60 seconds on every push, and for an hour nightly with a corpus that accumulates across runs. If a nightly run finds a crash, the reproducer input is uploaded as a workflow artifact.

If you have questions please create an issue at https://github.com/mas-bandwidth/serialize and I'll do my best to help you out.

cheers

 - Glenn
