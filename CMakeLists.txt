cmake_minimum_required(VERSION 3.16)

project(serialize VERSION 1.4.3 LANGUAGES CXX)

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
    # same flags as the historical premake build: extra warnings, fast float, no rtti
    if(MSVC)
        # 4127/4244 are opted back in here: the header now push/pops them, and this repo's own
        # executables use the same implicit-narrowing style as the header
        set(SERIALIZE_DEV_FLAGS /W4 /fp:fast /GR- /wd4127 /wd4244)
    else()
        set(SERIALIZE_DEV_FLAGS -Wall -Wextra -ffast-math -fno-rtti)
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
