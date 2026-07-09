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

If you have questions please create an issue at https://github.com/mas-bandwidth/serialize and I'll do my best to help you out.

cheers

 - Glenn
