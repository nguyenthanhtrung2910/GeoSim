# Repository Guidelines

## Project Structure & Module Organization

GeoSim is an Unreal Engine 5.8 C++ project, opened through `GeoSim.uproject`.

- `Source/GeoSim/`: gameplay, minimap, vegetation, buildings, and saved-type redirects.
- `Source/RoadGeneration/`: OSM import, road styles, topology, and Road Constructor integration. Public interfaces live in `Public/`; implementations and automation tests live in `Private/` and `Private/Tests/`.
- `Source/LandscapeGeneration/`: terrain downloading and landscape generation.
- `Plugins/`: integrations, including the LandscapeCombinator Git submodule. RoadConstructor must also be installed and enabled.
- `Content/`: Unreal assets, the Sevastopol map, and World Partition external actors. `Config/` contains project settings; `Docs/` explains generation workflows.

## Build, Test, and Development Commands

Run these PowerShell commands from the repository root; adjust the engine location locally:

```powershell
$UE = 'C:\Program Files\Epic Games\UE_5.8'
git submodule update --init --recursive
git lfs pull
& "$UE\Engine\Build\BatchFiles\Build.bat" GeoSimEditor Win64 Development "-Project=$PWD\GeoSim.uproject" -WaitMutex -NoHotReloadFromIDE
& "$UE\Engine\Binaries\Win64\UnrealEditor.exe" "$PWD\GeoSim.uproject"
```

The first two Git commands retrieve plugin sources and binary assets. The Unreal commands build the editor target and launch the project.

## Coding Style & Naming Conventions

Follow Unreal conventions: `A` actor, `U` UObject, `F` struct, and `E` enum prefixes; prefix booleans with `b`. Match surrounding indentation; existing C++ uses tabs and four-space indentation. Keep `.generated.h` last among header includes. Declare module dependencies in the relevant `.Build.cs`; guard editor-only code with `WITH_EDITOR`. No repository-wide formatter configuration is present. Check whitespace with `git diff --check`.

## Testing Guidelines

Use Unreal Automation tests guarded by `WITH_DEV_AUTOMATION_TESTS`. Follow existing names such as `Landscapes.RoadConstructor.LaneCounts`, `Landscapes.OSMRoadImporter.*`, and `GeoSim.ModuleRedirects.*`.

```powershell
& "$UE\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "$PWD\GeoSim.uproject" -unattended -nop4 -NullRHI '-ExecCmds=Automation RunTests Landscapes.RoadConstructor.LaneCounts+Landscapes.RoadConstructor.ImportTopology' '-TestExit=Automation Test Queue Empty'
```

Build before testing. Add regression coverage for changed behavior; no numerical coverage threshold is configured. Verify visible geometry changes in the editor.

## Commit & Pull Request Guidelines

History is limited; the visible subject `add RoadGeneration module.` uses a short action-oriented description. Use concise imperative subjects and focused commits. PRs should explain behavior changes, validation, related issues, and asset/configuration changes; include screenshots for visual changes. Preserve class redirects when renaming saved Unreal types. Keep generated caches and build outputs out of commits, and track binary assets through Git LFS.
