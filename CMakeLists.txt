cmake_minimum_required(VERSION 3.16)

project(serialize VERSION 1.11.0 LANGUAGES CXX)

include(GNUInstallDirs)

# is this the top level project, or pulled in via add_subdirectory / FetchContent?
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(SERIALIZE_TOP_LEVEL TRUE)
else()
    set(SERIALIZE_TOP_LEVEL FALSE)
endif()

option(SERIALIZE_BUILD_TESTS "Build serialize tests, example and benchmark" ${SERIALIZE_TOP_LEVEL})
option(SERIALIZE_INSTALL "Generate serialize install rules" ${SERIALIZE_TOP_LEVEL})
option(SERIALIZE_FUZZ "Build the libFuzzer harness (requires clang)" OFF)

# header-only library: consumers get the include path and nothing else — no compile flags leak
add_library(serialize INTERFACE)
add_library(serialize::serialize ALIAS serialize)
target_include_directories(serialize INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(SERIALIZE_INSTALL)
    include(CMakePackageConfigHelpers)
    install(FILES serialize.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(TARGETS serialize EXPORT serialize-targets)
    install(EXPORT serialize-targets
        FILE serialize-config.cmake
        NAMESPACE serialize::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/serialize
    )
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/serialize-config-version.cmake
        COMPATIBILITY SameMajorVersion
        ARCH_INDEPENDENT
    )
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/serialize-config-version.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/serialize
    )
endif()

# everything below builds the executables in this repo. flags here are scoped to those targets only.

if(SERIALIZE_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(SERIALIZE_BUILD_TESTS OR SERIALIZE_FUZZ)
    # STRICT floating point, deliberately. These executables are the gates that certify
    # the wire: the golden and conformance vectors pin exact bytes and exact decoded bit
    # patterns. The historical premake flags carried -ffast-math (MSVC /fp:fast) here,
    # which licenses contraction, reassociation and reciprocal approximation in exactly
    # the arithmetic those vectors exist to pin — measured on arm64, it changed both the
    # quantized integer (writer) and the decoded float (reader) of compressed_float.
    # A proof of byte-exactness earned under flags that permit different bytes is not a
    # proof, so every target here builds strict:
    #   - GCC/Clang: -ffp-contract=off. Not just "no -ffast-math" — GCC's default is
    #     -ffp-contract=fast, and clang's default =on still fuses single expressions.
    #   - MSVC: /fp:precise. Since VS 2022 this no longer implies contraction
    #     (there is no /fp:contract in these flags, so contraction stays off).
    # bench builds strict too: a benchmark of arithmetic the format forbids would be a
    # number about some other library. Consumers are unaffected either way — the library
    # target above is INTERFACE-only and none of these flags leak.
    if(MSVC)
        # 4127/4244 are opted back in here: the header now push/pops them, and this repo's own
        # executables use the same implicit-narrowing style as the header
        # /utf-8: this header has no BOM, and MSVC otherwise decodes a BOM-less UTF-8
        # source as the local ANSI code page — mangling non-ASCII characters and warning
        # C4819. The test data is written as explicit code points so behaviour never
        # depends on this, but the comments and the copyright line still need it.
        set(SERIALIZE_DEV_FLAGS /W4 /utf-8 /fp:precise /GR- /wd4127 /wd4244)
    else()
        set(SERIALIZE_DEV_FLAGS -Wall -Wextra -ffp-contract=off -fno-rtti)
    endif()
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

if(SERIALIZE_BUILD_TESTS)
    # "test" as a target name is reserved by CTest, so the target is serialize_test but the binary is still bin/test
    add_executable(serialize_test test.cpp serialize.h)
    target_compile_definitions(serialize_test PRIVATE SERIALIZE_ENABLE_TESTS=1)
    set_target_properties(serialize_test PROPERTIES OUTPUT_NAME test)

    add_executable(example example.cpp serialize.h)

    add_executable(bench bench.cpp serialize.h)

    foreach(target serialize_test example bench)
        target_link_libraries(${target} PRIVATE serialize)
        target_compile_options(${target} PRIVATE ${SERIALIZE_DEV_FLAGS})
        target_compile_definitions(${target} PRIVATE
            $<$<CONFIG:Debug>:SERIALIZE_DEBUG>
            $<$<NOT:$<CONFIG:Debug>>:SERIALIZE_RELEASE>
        )
    endforeach()

    enable_testing()
    add_test(NAME test COMMAND serialize_test)
endif()

# libFuzzer harness for the read side. requires clang. build in Debug so asserts stay live.
if(SERIALIZE_FUZZ)
    add_executable(fuzz fuzz.cpp serialize.h)
    target_link_libraries(fuzz PRIVATE serialize)
    target_compile_options(fuzz PRIVATE ${SERIALIZE_DEV_FLAGS} -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    target_compile_definitions(fuzz PRIVATE
        $<$<CONFIG:Debug>:SERIALIZE_DEBUG>
        $<$<NOT:$<CONFIG:Debug>>:SERIALIZE_RELEASE>
    )
endif()
