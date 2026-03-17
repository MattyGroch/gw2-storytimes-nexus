# GW2 Story Times for Nexus

This repository contains the Raidcore Nexus port of the original BlishHUD `gw2-storytimerwidget` module.

Current capabilities:

- Nexus addon lifecycle (`GetAddonDef`, `Load`, `Unload`)
- Compact always-on timer widget
- Pop-out mission browser
- Live data loading from `api.gw2storytimes.com`
- Stopwatch state and pacing feedback
- Time submission support
- GitHub-based update metadata for Nexus

## Current Status

The addon is now in a playable prototype state and uses the live GW2 Story Times API. It is still not feature-complete compared to the BlishHUD version, but the core browse / time / submit loop is in place.

Release target: `v0.1.2`

## Build

### Prerequisites

- Visual Studio 2022 with Desktop C++ tools
- CMake 3.21+
- A local Nexus SDK/include tree

This repository currently defaults to the cloned reference copy in [`_reference_nexus`](./_reference_nexus), which was pulled in during scaffolding. If you want to point at a different SDK location, override these cache variables:

- `NEXUS_INCLUDE_DIR`
- `IMGUI_INCLUDE_DIR`

### Configure

Use a Visual Studio Developer Command Prompt, or run `VsDevCmd.bat` before building from a normal shell.

```powershell
cmake -S . -B build -G "NMake Makefiles"
cmake --build build
```

For release builds:

```powershell
cmake -S . -B build-release -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

The release addon DLL will be produced as `build-release/gw2storytimes.dll`.

## Porting Roadmap

1. Add persistent settings for window visibility, preferred category, and default submission behavior.
2. Register a Quick Access shortcut with addon icon assets.
3. Read Mumble/Nexus character context so My Story race filtering behaves like the BlishHUD module.
4. Improve browser presentation to preserve story grouping and chapter headers visually.
5. Package and test inside a local Nexus install.

## Source Reference

The original BlishHUD module is available here:

- [https://github.com/MattyGroch/gw2-storytimerwidget](https://github.com/MattyGroch/gw2-storytimerwidget)
