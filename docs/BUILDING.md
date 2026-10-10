# Building from source

Players don't need this: install the mod from a mod manager (see the [README](../README.md)).

## Requirements

Install these once:

- The [.NET SDK](https://dotnet.microsoft.com/download) (the runtime alone is not enough).
- [Git for Windows](https://git-scm.com/download/win).
- Visual Studio or Build Tools with **Desktop development with C++**, the **Windows SDK** and **CMake tools** (or standalone CMake 3.24+).
- Risk of Rain 2, and an r2modman (or Thunderstore Mod Manager) profile with BepInEx 5 and [Risk of Options](https://thunderstore.io/package/Rune580/Risk_Of_Options/). The game and profile supply build references; later builds can use cached references.

The build runs on Windows PowerShell 5.1 and uses PowerShell 7 when available.

## Build

Double-click [Build.cmd](../Build.cmd). It finds the game, a usable mod profile and the build tools, downloads the pinned Streamline and FidelityFX SDK files on first use, and creates `build/package`. It also copies the built `BepInEx` folder into [Package](../Package) next to the Thunderstore manifest, README and icon, copies [CHANGELOG.md](../CHANGELOG.md) there, and sets the manifest's `version_number` to the plugin version from [Directory.Build.props](../Directory.Build.props). The window stays open with the result; the full log is saved to `build/Build.log`.

Automatic profile selection prefers a profile that already has this mod, then the newest usable profile.

### Custom locations

Copy [build/Build.local.psd1.example](../build/Build.local.psd1.example) to `build/Build.local.psd1` and fill in the paths you need. Command-line overrides also work:

```bat
Build.cmd -NoPause -GamePath "D:\SteamLibrary\steamapps\common\Risk of Rain 2" -ProfilePath "C:\Mods\My profile"
```

Put `-NoPause` first for unattended builds.

## Install a build into a profile

In PowerShell 7, run [build/Install-Profile.ps1](../build/Install-Profile.ps1). It uses the same profile discovery and `Build.local.psd1` settings as the build, or `-ProfilePath`. It verifies the package hashes, backs up the previous mod files, and preserves your existing config. Launch with the mod manager's **Start modded**.

## Package layout

```
BepInEx/
  patchers/RiskOfRainUpscaling/   preloader + native DLL (early graphics hooks)
  plugins/RiskOfRainUpscaling/    plugin DLL, runtimes/{D3D11,D3D12,FSR}, licenses
```

Mod-manager `Author-Package` folder names are supported: `runtimes` lives beside `RiskOfRainUpscaling.dll`, and the native DLL beside the preloader.

## Tests

`build/Test.ps1` runs the managed tests, the native fixtures, the startup test and the profile install test. `-FrameGeneration` and `-ReflexLimiter` add interactive hardware Frame Generation runs that open windows and need a focused desktop.

Manual test procedures for each feature are in [docs/testing](testing):

- [Upscaling](testing/UPSCALING_TESTING.md)
- [Frame Generation and pacing](testing/FRAME_GENERATION_TESTING.md)
- [FSR](testing/FSR_TESTING.md)
- [UI recomposition](testing/UI_RECOMPOSITION_TESTING.md)
- [Options](testing/OPTIONS_TESTING.md)
- [Startup](testing/STARTUP_TESTING.md)

## More

- [Architecture](ARCHITECTURE.md): source layout, ABI conventions, frame timeline.
- [Diagnostics](DIAGNOSTICS.md): logs, debug overlay and capture.
- [Third-party notices](../THIRD_PARTY_NOTICES.md): bundled SDKs and their licenses.
