# Dante's Devil Trigger (SKSE plugin)

Source of `DanteDevilTrigger.dll`, the SKSE plugin of
[Dante's Devil Trigger](https://www.nexusmods.com/skyrimspecialedition/mods/103644) on Nexus Mods (version 2.0).
The plugin implements the mod natively (no Papyrus scripts): Devil Trigger / Sin Devil Trigger transformations,
the costume, the magicka cost, the Luce, and the shout and its words on game load.

The DLL alone does nothing: it looks up forms in `OmecaOneDanteSDT.esp`, which ships with the Nexus release
together with the meshes, textures, sounds, Open Animation Replacer sections and `OmecaOneDanteSDT.ini`.

## Runtime requirements
- Skyrim Anniversary Edition 1.7.x (built and tested on 1.7.104). 1.6.x and SE 1.5.97 are not supported; ports welcome
  (see [Porting](#porting)).
- [SKSE64](https://skse.silverlock.org/)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
- Optional: [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352) (in-game settings page),
  [True Directional Movement](https://www.nexusmods.com/skyrimspecialedition/mods/51614) (lock-on target for the Luce).

The mod's other requirements (animations, effects, physics) are listed on the Nexus page.

## Building
Requirements:
- [XMake](https://xmake.io) 3.0.0+
- MSVC with C++23 support (Visual Studio 2022 17.x or newer)

```bat
git clone --recurse-submodules <this repository's URL>
cd <repository folder>
xmake f -m releasedbg -y
xmake
```

The DLL and PDB end up in `build\windows\x64\releasedbg\`.
If the environment variable `XSE_TES5_MODS_PATH` points to a mod manager's `mods` folder, `xmake install`
copies the DLL into the mod folder `Dante's Devil Trigger` there (another name: `xmake f --mod_name="<name>"`).

## Layout
| File | Contents |
|---|---|
| `src/main.cpp` | SKSE entry point, messaging, per-frame update of transformed actors |
| `src/settings.cpp` | reads/writes `OmecaOneDanteSDT.ini`, applies values to perks, effects and globals |
| `src/menu.cpp` | SKSE Menu Framework settings page |
| `src/transform.cpp` | transformation stages (DT / SDT / off), end conditions, untransform animation, triggers from animation clips, shout setup on game load |
| `src/visual.cpp` | visual-only costume drawn over the worn gear |
| `src/cost.cpp` | magicka drain and activation cost |
| `src/luce.cpp` | the Luce (thrown sword) |
| `src/TrueDirectionalMovementAPI.h` | True Directional Movement API header |
| `include/SKSEMCP/` | SKSE Menu Framework header ([SKSE-MCP](https://github.com/QTR-Modding/SKSE-MCP), MIT) |
| `lib/commonlibsse` | [libxse CommonLibSSE](https://github.com/libxse/commonlibsse) (submodule) |

## Porting
The plugin is built against [libxse CommonLibSSE](https://github.com/libxse/commonlibsse), which uses the 1.7.x struct layouts only.
SKSE for 1.6.x would load the DLL (it uses Address Library), but members that moved in 1.7 would be read from the wrong place.
- 1.6.1170: build against a CommonLib that targets it (an older libxse commit, or
  [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG), which handles SE/AE/VR at runtime; with NG, replace the
  libxse-specific `REX::` logging and Win32 calls).
- SE 1.5.97: CommonLibSSE-NG only; every engine function and hook used needs an SE equivalent.

## License
GPL-3.0 with the modding exceptions in [`EXCEPTIONS`](EXCEPTIONS) (from the libxse plugin template).
The third-party headers (`include/SKSEMCP/`, `src/TrueDirectionalMovementAPI.h`) belong to their authors and are under their own terms.
