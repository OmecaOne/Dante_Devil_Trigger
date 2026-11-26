-- include subprojects
includes("lib/commonlibsse")

-- set project constants
set_project("DanteDevilTrigger")
set_version("2.0.1")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- mod folder name for `xmake install` (xmake f --mod_name="...")
option("mod_name")
    set_default("Dante's Devil Trigger")
    set_showmenu(true)
    set_description("Mod folder under XSE_TES5_MODS_PATH that `xmake install` copies the plugin into")
option_end()

-- define targets
target("DanteDevilTrigger")
    add_rules("commonlibsse.plugin", {
        name = "DanteDevilTrigger",
        author = "OmecaOne",
        description = "Dante's Devil Trigger: DT / SDT transformations, costume, magicka cost and the Luce"
    })

    -- `xmake install` copies the plugin into XSE_TES5_MODS_PATH/<mod_name> (a mod manager's mods folder)
    on_config(function(target)
        local mods = os.getenv("XSE_TES5_MODS_PATH")
        if mods then
            target:set("installdir", path.join(mods, get_config("mod_name") or "Dante's Devil Trigger"))
        end
    end)

    -- the commonlibsse rule installs the PDB (~21 MB) next to the DLL: shipped with the mod, so crash logs show symbol names

    -- add src files
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    add_includedirs("include")  -- SKSEMCP\SKSEMenuFramework.hpp (SKSE-MCP f95f268, MIT)
    add_defines("_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING")  -- that header's UTF-8 helper uses <codecvt>
    set_pcxxheader("src/pch.h")
