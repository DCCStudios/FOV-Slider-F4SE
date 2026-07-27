-- One DLL supports Fallout 4 OG, NG, and AE.
includes("lib/commonlibf4")

set_project("FOVSliderF4SE")
set_version("1.0.0")
set_license("MIT")
set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")
set_allowedarchs("windows|x64")
set_allowedmodes("debug", "releasedbg")
set_defaultarchs("windows|x64")
set_defaultmode("releasedbg")

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- REL::ID and REL::Offset initializer slots are [OG, NG, AE].
add_defines("COMMONLIB_RUNTIMECOUNT=3")

target("FOVSliderF4SE", function()
    add_rules("commonlibf4.plugin", {
        name = "FOVSliderF4SE",
        author = "Robert",
        description = "Runtime FOV controls for Fallout 4.",
        plugin_template = path.join(os.projectdir(), "res/commonlibf4-plugin.cpp.in"),
    })

    add_files("src/**.cpp")
    add_headerfiles("src/**.h", "include/**.h")
    add_includedirs("src", "include")

    add_defines(
        "_UNICODE",
        "UNICODE",
        "NOMINMAX",
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING"
    )

    add_cxxflags(
        "/sdl",
        "/Zi",
        "/permissive-",
        "/Zc:preprocessor",
        "/EHsc",
        "/wd4099",
        "/wd4100",
        "/wd4189",
        "/wd4244",
        "/wd4302",
        "/wd4311",
        "/wd5054"
    )

    set_pcxxheader("include/PCH.h")
    set_runtimes("MD")
    set_symbols("debug")
    set_optimize("fastest")
    set_targetdir("Compile/F4SE/Plugins")

    after_build(function(target)
        os.cp("FOV Slider F4SE.ini", target:targetdir())
    end)
end)
