cmake_minimum_required(VERSION 3.16)

# A no-op build prints nothing (rowan-new#110). The per-target "Built target" lines are
# the only output a rebuild with nothing to do produces, and they carry no information; a
# real build still prints its [ x%] progress lines, and errors are untouched. This sits
# above project() because CMAKE_TARGET_MESSAGES initializes the TARGET_MESSAGES global
# property there and has no effect if set after it, and under the same top-level test as
# SERIALIZE_TOP_LEVEL below, so a consumer that pulls this library in through FetchContent
# keeps its own messages.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(CMAKE_TARGET_MESSAGES OFF)
endif()

project(serialize VERSION 1.16.1 LANGUAGES CXX)

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
    # SameMajorVersion states API compatibility and says nothing about the wire.
    # Wire agreement between two endpoints is the format version at the head of
    # STANDARD.md, which a library release states separately.
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
    # Consumers are unaffected either way — the library target above is INTERFACE-only
    # and none of these flags leak. The wire itself no longer depends on any of this:
    # the quantization's roundings are pinned in-source by SERIALIZE_FLOAT_FORCE_ROUND,
    # and the -ffp-contract=fast test target below proves the bytes hold at the loosest
    # contraction setting. The strict flags remain the estate's
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

    # The shared conformance corpus, run through this library's reader. The vector files are named
    # on the command line rather than discovered at run time, because a directory walk is not
    # portable across this repo's three platforms and the glob belongs at configure time anyway.
    # GLOB so a new operation's file is picked up by re-running cmake; CONFIGURE_DEPENDS so an
    # added or removed file re-runs it on the next build.
    file(GLOB SERIALIZE_CONFORMANCE_VECTORS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/conformance/*.txt)
    if(NOT SERIALIZE_CONFORMANCE_VECTORS)
        message(FATAL_ERROR "conformance/ holds no vector files. The corpus is the conformance instrument; an empty one is a broken checkout, not a pass.")
    endif()
    add_executable(serialize_conformance conformance.cpp serialize.h)
    set_target_properties(serialize_conformance PROPERTIES OUTPUT_NAME conformance)

    # The write side debug assertions, observed rather than aborted on: asserts.cpp defines
    # serialize_assert itself, so the assertions are live in Release too and this target is
    # registered unconditionally.
    add_executable(serialize_asserts asserts.cpp serialize.h)
    set_target_properties(serialize_asserts PROPERTIES OUTPUT_NAME asserts)

    foreach(target serialize_test example serialize_conformance serialize_asserts)
        target_link_libraries(${target} PRIVATE serialize)
        target_compile_options(${target} PRIVATE ${SERIALIZE_DEV_FLAGS})
        target_compile_definitions(${target} PRIVATE
            $<$<CONFIG:Debug>:SERIALIZE_DEBUG>
            $<$<NOT:$<CONFIG:Debug>>:SERIALIZE_RELEASE>
        )
    endforeach()

    enable_testing()
    add_test(NAME test COMMAND serialize_test)
    add_test(NAME conformance COMMAND serialize_conformance ${SERIALIZE_CONFORMANCE_VECTORS})

    # NEGATIVE CONTROLS. The corpus is the conformance instrument, and an instrument nobody has
    # seen go red is a claim rather than a measurement. Each control below is a deliberately
    # broken input the gate MUST reject; ctest's WILL_FAIL inverts the verdict, so a control test
    # passes exactly when the gate it guards goes red. If one of these ever starts failing, the
    # gate has stopped discriminating and every green result it has produced is worth
    # re-examining.

    # Control 1: a corpus file whose operation has no runner. STANDARD.md requires a suite to run
    # every vector in the corpus, so a runner that quietly skips what it does not understand
    # reports green over a rule nobody checked. The vector lives in conformance-controls/, which
    # the corpus glob above deliberately does not reach. The Go checker is run against the same
    # file in CI, because the contract binds both gates and not only this one.
    add_test(NAME conformance-control-unknown-operation
             COMMAND serialize_conformance ${CMAKE_CURRENT_SOURCE_DIR}/conformance-controls/unknown-operation.txt)
    set_tests_properties(conformance-control-unknown-operation PROPERTIES WILL_FAIL TRUE)

    # Control 2: a reader with ONE LINE of a refusal rule deleted, built from a scratch copy of
    # the header and run against the real corpus, which must go red. This is the control that
    # answers "would the corpus notice if the implementation were wrong?" — and it answers it per
    # rule, because each sabotage names the rule it deletes and the vectors that pin it.
    #
    # The scratch copy is generated at configure time and the sabotage is a string replacement, so
    # a needle that stops matching is a FATAL_ERROR rather than a silent no-op: a control that has
    # quietly stopped sabotaging anything is worse than no control at all. conformance.cpp is
    # copied alongside the sabotaged header because it includes "serialize.h" with quotes, which
    # resolves against the including file's own directory first.
    set(SERIALIZE_CONTROL_NAMES align-padding ranged-reconstruction)
    set(SERIALIZE_CONTROL_NEEDLE_align-padding
        "                if ( value != 0 )")
    set(SERIALIZE_CONTROL_RULE_align-padding
        "readers must verify that the alignment padding bits are zero")
    # Deliberately the RECONSTRUCTION and not the range check beside it. The ranged int's range
    # rule is guarded twice — once in ReadStream::SerializeInteger against the offset, and again
    # in the serialize_int macro against the decoded value — so deleting either guard alone
    # changes no verdict and no vector can see it. A control aimed there would report green while
    # proving nothing, which is the failure these controls exist to prevent. Dropping min from the
    # reconstruction is a single line with no second guard behind it.
    set(SERIALIZE_CONTROL_NEEDLE_ranged-reconstruction
        "            value = int32_t( unsigned_value + uint32_t(min) );")
    set(SERIALIZE_CONTROL_RULE_ranged-reconstruction
        "a ranged int decodes to its offset plus min")
    # each replacement keeps the sabotaged line compiling warning-free: the padding is at most
    # seven bits, so comparing it against 0xFFFFFFFF is a condition that never holds while still
    # reading the variable the deleted rule read
    set(SERIALIZE_CONTROL_PATCH_align-padding
        "                if ( value == 0xFFFFFFFF ) // NEGATIVE CONTROL: this rule is deleted on purpose")
    set(SERIALIZE_CONTROL_PATCH_ranged-reconstruction
        "            value = int32_t( unsigned_value ); // NEGATIVE CONTROL: min is dropped on purpose")

    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/serialize.h SERIALIZE_HEADER_TEXT)
    foreach(control ${SERIALIZE_CONTROL_NAMES})
        set(needle "${SERIALIZE_CONTROL_NEEDLE_${control}}")
        string(REPLACE "${needle}" "${SERIALIZE_CONTROL_PATCH_${control}}"
               SERIALIZE_SABOTAGED_TEXT "${SERIALIZE_HEADER_TEXT}")
        if(SERIALIZE_SABOTAGED_TEXT STREQUAL SERIALIZE_HEADER_TEXT)
            message(FATAL_ERROR
                "conformance control '${control}' found nothing to sabotage in serialize.h. The line it "
                "deletes has moved or changed, so the control is no longer proving that deleting "
                "'${SERIALIZE_CONTROL_RULE_${control}}' turns the corpus red. Update the needle in "
                "CMakeLists.txt; do not delete the control.")
        endif()
        set(dir ${CMAKE_BINARY_DIR}/conformance-controls/${control})
        file(MAKE_DIRECTORY ${dir})
        file(WRITE ${dir}/serialize.h "${SERIALIZE_SABOTAGED_TEXT}")
        configure_file(${CMAKE_CURRENT_SOURCE_DIR}/conformance.cpp ${dir}/conformance.cpp COPYONLY)

        add_executable(serialize_conformance_control_${control} ${dir}/conformance.cpp ${dir}/serialize.h)
        set_target_properties(serialize_conformance_control_${control} PROPERTIES OUTPUT_NAME conformance-control-${control})
        target_compile_options(serialize_conformance_control_${control} PRIVATE ${SERIALIZE_DEV_FLAGS})
        # The controls always build UNCHECKED. A reader with a rule deleted produces values a
        # checked build asserts on, and an abort is a hard ctest failure that WILL_FAIL does not
        # invert — so a control built with assertions live would report a crash where the thing
        # under test is the corpus's verdict. What must go red here is the corpus, cleanly.
        target_compile_definitions(serialize_conformance_control_${control} PRIVATE SERIALIZE_RELEASE NDEBUG)
        add_test(NAME conformance-control-${control}
                 COMMAND serialize_conformance_control_${control} ${SERIALIZE_CONFORMANCE_VECTORS})
        set_tests_properties(conformance-control-${control} PROPERTIES WILL_FAIL TRUE)
    endforeach()
    add_test(NAME asserts COMMAND serialize_asserts)

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
