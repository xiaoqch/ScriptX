add_rules("mode.debug", "mode.release")

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

if is_config("backend", "Lua") then
    add_requires("lua v5.4.7", {configs={shared=true}})

elseif is_config("backend", "QuickJs") then
    add_requires("quickjs-ng v0.8.0", {configs={shared=true, libc=true}})

elseif is_config("backend", "Python") then
    add_requires("python 3.12.8", {configs={shared=true}})

elseif is_config("backend", "V8") then
    add_requires("node v22.12.0", {configs={shared=true}})
    
end

option("backend")
    set_default("Lua")
    set_values("Lua", "QuickJs", "Python", "V8")

target("ScriptX")
    add_files(
        "src/**.cc"
    )
    add_headerfiles(
        "(**.h)",
        "(**.hpp)"
    )
    add_includedirs(
        "src/include/"
    )
    set_kind("static")
    set_languages("cxx20")

    if is_config("backend", "Lua") then
        add_defines(
            "SCRIPTX_BACKEND_LUA",
            "SCRIPTX_BACKEND_TRAIT_PREFIX=../backend/Lua/trait/Trait"
        )
        add_files(
            "backend/Lua/**.cc"
        )
        add_packages(
            "lua"
        )

    elseif is_config("backend", "QuickJs") then
        add_defines(
            "SCRIPTX_BACKEND_QUICKJS",
            "SCRIPTX_BACKEND_TRAIT_PREFIX=../backend/QuickJs/trait/Trait"
        )
        add_files(
            "backend/QuickJs/**.cc"
        )
        add_packages(
            "quickjs-ng"
        )

    elseif is_config("backend", "Python") then
        add_defines(
            "SCRIPTX_BACKEND_PYTHON",
            "SCRIPTX_BACKEND_TRAIT_PREFIX=../backend/Python/trait/Trait"
        )
        add_files(
            "backend/Python/**.cc",
            "backend/Python/**.c"
        )
        add_packages(
            "python"
        )

    elseif is_config("backend", "V8") then
        add_defines(
            "SCRIPTX_BACKEND_V8",
            "SCRIPTX_BACKEND_TRAIT_PREFIX=../backend/V8/trait/Trait"
        )
        add_files(
            "backend/V8/**.cc"
        )
        add_packages(
            "node"
        )

    end
