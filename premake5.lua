
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
    files { "test.cpp", "include/serialize.h" }
    defines { "SERIALIZE_ENABLE_TESTS=1" }

project "example"
    files { "example.cpp", "include/serialize.h" }

newaction
{
    trigger     = "clean",

    description = "Clean all build files and output",

    execute = function ()

        files_to_delete = 
        {
            "Makefile",
            "*.make",
            "*.txt",
            "*.zip",
            "*.tar.gz",
            "*.db",
            "*.opendb",
            "*.vcproj",
            "*.vcxproj",
            "*.vcxproj.user",
            "*.vcxproj.filters",
            "*.sln",
            "*.xcodeproj",
            "*.xcworkspace"
        }

        directories_to_delete = 
        {
            "obj",
            "ipch",
            "bin",
            ".vs",
            "Debug",
            "Release",
            "release",
            "cov-int",
            "docs",
            "xml"
        }

        for i,v in ipairs( directories_to_delete ) do
          os.rmdir( v )
        end

        if not os.istarget "windows" then
            os.execute "find . -name .DS_Store -delete"
            for i,v in ipairs( files_to_delete ) do
              os.execute( "rm -f " .. v )
            end
        else
            for i,v in ipairs( files_to_delete ) do
              os.execute( "del /F /Q  " .. v )
            end
        end

    end
}
