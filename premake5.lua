
solution "Serialize"
    kind "ConsoleApp"
    language "C++"
    configurations { "Debug", "Release" }
    includedirs { "." }
    if not os.istarget "windows" then
        targetdir "bin/"  
    end
    rtti "Off"
    warnings "Extra"
    flags { "FatalWarnings" }
    floatingpoint "Fast"
    filter "configurations:Debug"
        symbols "On"
        defines { "SERIALIZE_DEBUG" }
    filter "configurations:Release"
        symbols "Off"
        optimize "Speed"
        defines { "SERIALIZE_RELEASE" }

project "test"
    files { "test.cpp", "serialize.h" }
    defines { "SERIALIZE_ENABLE_TESTS=1" }

project "example"
    files { "example.cpp", "serialize.h" }
