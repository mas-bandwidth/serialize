cmake_minimum_required(VERSION 3.16)

project(serialize LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# header-only library
add_library(serialize INTERFACE)
target_include_directories(serialize INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# same flags as the old premake build: extra warnings, fast float, no rtti
if(MSVC)
    add_compile_options(/W4 /fp:fast /GR-)
else()
    add_compile_options(-Wall -Wextra -ffast-math -fno-rtti)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# "test" as a target name is reserved by CTest, so the target is serialize_test but the binary is still bin/test
add_executable(serialize_test test.cpp serialize.h)
target_link_libraries(serialize_test PRIVATE serialize)
target_compile_definitions(serialize_test PRIVATE SERIALIZE_ENABLE_TESTS=1)
set_target_properties(serialize_test PROPERTIES OUTPUT_NAME test)

add_executable(example example.cpp serialize.h)
target_link_libraries(example PRIVATE serialize)

foreach(target serialize_test example)
    target_compile_definitions(${target} PRIVATE
        $<$<CONFIG:Debug>:SERIALIZE_DEBUG>
        $<$<NOT:$<CONFIG:Debug>>:SERIALIZE_RELEASE>
    )
endforeach()

enable_testing()
add_test(NAME test COMMAND serialize_test)
