cmake_minimum_required(VERSION 3.16)

project(serialize VERSION 1.15.0 LANGUAGES CXX)

include(GNUInstallDirs)

# is this the top level project, or pulled in via add_subdirectory / FetchContent?
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(SERIALIZE_TOP_LEVEL TRUE)
else()
    set(SERIALIZE_TOP_LEVEL FALSE)
endif()

option(SERIALIZE_BUILD_TESTS "Build serialize tests and example" ${SERIALIZE_TOP_LEVEL})
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
    # Consumers are unaffected either way — the library target above is INTERFACE-only and none of these flags leak. The wire itself no
    # longer depends on any of this: the quantization's roundings are pinned in-source by
    # SERIALIZE_FLOAT_FORCE_ROUND, and the -ffp-contract=fast test target below proves the
    # bytes hold at the loosest contraction setting. The strict flags remain the estate's
    # standing policy for network libraries (belt), and -ffast-math stays unsupported —
    # reciprocal approximation breaks the vectors and no barrier can repair it.
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

    foreach(target serialize_test example)
        target_link_libraries(${target} PRIVATE serialize)
        target_compile_options(${target} PRIVATE ${SERIALIZE_DEV_FLAGS})
        target_compile_definitions(${target} PRIVATE
            $<$<CONFIG:Debug>:SERIALIZE_DEBUG>
            $<$<NOT:$<CONFIG:Debug>>:SERIALIZE_RELEASE>
        )
    endforeach()

    enable_testing()
    add_test(NAME test COMMAND serialize_test)

    # THE SAME SUITE A SECOND TIME AT -ffp-contract=on, AND A THIRD TIME AT
    # -ffp-contract=fast. This is not belt and braces; these are the only builds in which the
    # compressed float's FMA discipline has teeth.
    #
    # That discipline is a pair of locals pinned by SERIALIZE_FLOAT_FORCE_ROUND, which keep
    # the quantization's two roundings distinct under EVERY contraction mode -- that is what
    # makes the wire bits independent of the consumer's compiler flags (issues #92/#95).
    # Whether DAMAGING the discipline is visible depends entirely on the contraction setting,
    # measured on clang 21 / arm64 by falsifying the audited home and rebuilding, all four
    # falsifications, all three modes:
    #
    #                                  =off    =on     =fast
    #    reader barrier deleted        PASS    PASS    RED (nonzero-min vector)
    #    reader folded to one stmt     PASS    RED     RED
    #    writer barrier deleted        PASS    PASS    RED (writer-fusion vector)
    #    writer folded to one stmt     PASS    RED     RED
    #
    # =off is blind to all four: contraction is forbidden everywhere, so every spelling
    # computes the same thing. =on sees the one-expression folds and cannot see a deleted
    # barrier -- statement-local contraction never fuses through any local. =fast sees
    # everything, because it is the mode in which only the barrier stands between the two
    # roundings and one. So the targets above stay at -ffp-contract=off, because that is the
    # certification build, and these two add back every row of the discrimination it costs.
    # The =fast build is also the consumer reality check: it is GCC's default at every
    # optimization level, and the exact build in which arm64 consumers shipped divergent
    # wire before the barrier existed. mas-bandwidth/schema#82; serialize.c#38 is the =on
    # build in the C port.
    #
    # The contract flag comes AFTER ${SERIALIZE_DEV_FLAGS} so it wins over the =off in there,
    # including when CI overrides CMAKE_CXX_FLAGS wholesale (the sanitizers job does). Nothing
    # new is asked of the compiler: the dev flags already spell -ffp-contract, so any compiler
    # that can build the suite at all can build these. MSVC is excluded because cl has no such
    # flag -- and needs none: it is the one configuration in the matrix that never receives
    # -ffp-contract=off, so the first build is already the library's default-contraction
    # behaviour executed rather than reasoned about.
    #
    # The binary reports whether contraction is actually LIVE on the target -- x86-64 without
    # FMA cannot fuse however the flag is set, so on such hosts the =fast leg exercises
    # nothing beyond =off and the real coverage comes from FMA targets (the macos arm64 legs
    # in CI, and any arm64 or -mfma consumer build). It also detects the one compiler trap
    # here: GCC before 14 mapped -ffp-contract=on onto =fast, so on gcc 13 asking for
    # statement-local contraction gets cross-statement contraction instead.
    # SERIALIZE_TEST_FP_CONTRACT_REQUESTED_ON tells the binary this build asked for =on, and
    # it stands down with a printed reason instead of certifying a discipline it is not
    # running; the =fast target carries the cross-statement coverage on such a toolchain.
    if(NOT MSVC)
        add_executable(serialize_test_fp_contract_on test.cpp serialize.h)
        set_target_properties(serialize_test_fp_contract_on PROPERTIES OUTPUT_NAME test-fp-contract-on)
        target_link_libraries(serialize_test_fp_contract_on PRIVATE serialize)
        target_compile_options(serialize_test_fp_contract_on PRIVATE ${SERIALIZE_DEV_FLAGS} -ffp-contract=on)
        target_compile_definitions(serialize_test_fp_contract_on PRIVATE
            SERIALIZE_ENABLE_TESTS=1
            SERIALIZE_TEST_FP_CONTRACT="-ffp-contract=on"
            SERIALIZE_TEST_FP_CONTRACT_REQUESTED_ON=1
            $<$<CONFIG:Debug>:SERIALIZE_DEBUG>
            $<$<NOT:$<CONFIG:Debug>>:SERIALIZE_RELEASE>
        )
        add_test(NAME test-fp-contract-on COMMAND serialize_test_fp_contract_on)

        add_executable(serialize_test_fp_contract_fast test.cpp serialize.h)
        set_target_properties(serialize_test_fp_contract_fast PROPERTIES OUTPUT_NAME test-fp-contract-fast)
        target_link_libraries(serialize_test_fp_contract_fast PRIVATE serialize)
        target_compile_options(serialize_test_fp_contract_fast PRIVATE ${SERIALIZE_DEV_FLAGS} -ffp-contract=fast)
        target_compile_definitions(serialize_test_fp_contract_fast PRIVATE
            SERIALIZE_ENABLE_TESTS=1
            SERIALIZE_TEST_FP_CONTRACT="-ffp-contract=fast"
            $<$<CONFIG:Debug>:SERIALIZE_DEBUG>
            $<$<NOT:$<CONFIG:Debug>>:SERIALIZE_RELEASE>
        )
        add_test(NAME test-fp-contract-fast COMMAND serialize_test_fp_contract_fast)
    endif()
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
