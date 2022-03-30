local luadir = "./3rd/lua/"
local dir3rd = "./3rd/"
local cjsondir = dir3rd.."cjson/"

workspace "pipes"
    configurations { "Debug", "Release" }
    flags{"NoPCH","RelativeLinks"}
    location "./"
    architecture "x64"

    filter "configurations:Debug"
        defines { "DEBUG" }
        symbols "On"

    filter "configurations:Release"
        defines { "NDEBUG" }
        optimize "On"
        -- symbols "On" -- add debug symbols

    filter {"system:windows"}
        characterset "MBCS"
        systemversion "latest"
        warnings "Extra"
        -- staticruntime "on" -- static link vc runtime

    filter { "system:linux" }
        warnings "High"

project "lua54"
    location "build/projects/%{prj.name}"
    objdir "build/obj/%{prj.name}/%{cfg.buildcfg}"
    targetdir "build/bin/%{cfg.buildcfg}"
    kind "SharedLib"
    language "C"
    includedirs {luadir}
    files { luadir.."**.h", luadir.."**.c"}
    removefiles(luadir.."luac.c")
    removefiles(luadir.."lua.c")
    filter { "system:windows" }
        defines {"LUA_BUILD_AS_DLL"}
    filter { "system:linux" }
        defines {"LUA_USE_LINUX"}
    filter{"configurations:*"}
        postbuildcommands{"{COPY} %{cfg.buildtarget.abspath} %{wks.location}"}

project "lua"
    location "build/projects/%{prj.name}"
    objdir "build/obj/%{prj.name}/%{cfg.buildcfg}"
    targetdir "build/bin/%{cfg.buildcfg}"
    kind "ConsoleApp"
    language "C"
    includedirs {luadir}
    files { luadir.."lua.c"}
    links{"lua54"}
    filter { "system:windows" }
        defines {"LUA_BUILD_AS_DLL"}
    filter { "system:linux" }
        defines {"LUA_USE_LINUX"}
        links{"dl","pthread", "m"}
        linkoptions {"-Wl,-rpath,./"}
    filter{"configurations:*"}
        postbuildcommands{"{COPY} %{cfg.buildtarget.abspath} %{wks.location}"}

project "pipesc"
    location "build/projects/%{prj.name}"
    objdir "build/obj/%{prj.name}/%{cfg.buildcfg}"
    targetdir "build/bin/%{cfg.buildcfg}"
    kind "SharedLib"
    language "C++"
    targetprefix ""
    includedirs {luadir, dir3rd, "src"}
    files {"./src/**.h", "./src/**.cpp", cjsondir.."*.c" }
    links{"lua54"}
    filter { "system:windows" }
        defines {"LUA_BUILD_AS_DLL"}
        links{"winmm"}
    filter{"configurations:*"}
        postbuildcommands{"{COPY} %{cfg.buildtarget.abspath} %{wks.location}"}
        