cmake_minimum_required(VERSION 3.19.0 FATAL_ERROR)

option(BUILD_EXAMPLE "Build example" ON)
option(BUILD_TEST "Build test" ON)
option(SERIALIZE_INSTALL "Generate the install target." ON)

project(serialize VERSION 1.0 LANGUAGES CXX)

set(Header_Files
	"include/serialize.h"
)
source_group("Header Files" FILES ${Header_Files})

include(GNUInstallDirs)

add_library(serialize INTERFACE ${Header_Files})

target_include_directories(serialize INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:/include>)

			
string(APPEND CMAKE_CXX_FLAGS_DEBUG_INIT " -g SERIALIZE_DEBUG")
string(APPEND CMAKE_CXX_FLAGS_RELEASE_INIT " -O3 -DNDEBUG SERIALIZE_RELEASE")

if (BUILD_TEST)
	add_executable(test "test.cpp")
	target_compile_definitions(test PRIVATE
		"$<$<CONFIG:Debug>:"
            "SERIALIZE_DEBUG;"
        ">"
		"$<$<CONFIG:Release>:"
            "SERIALIZE_RELEASE;"
        ">"
		SERIALIZE_ENABLE_TESTS=1
        "_CONSOLE;"
    )
	target_link_libraries(test serialize)
endif ()
if (BUILD_EXAMPLE)
	add_executable(example "example.cpp")
	target_compile_definitions(example PRIVATE
		"$<$<CONFIG:Debug>:"
			"SERIALIZE_DEBUG;"
            "_DEBUG;"
        ">"
		"$<$<CONFIG:Release>:"
			"SERIALIZE_RELEASE;"
            "NDEBUG;"
        ">"
        "_CONSOLE;"
    )
	target_link_libraries(example serialize)
endif ()


if (SERIALIZE_INSTALL)
	install(TARGETS serialize)
endif()