# Preloader startup testing

The BepInEx preloader loads its adjacent `RiskOfRainUpscaling.Native.dll` and calls `RiskOfRainUpscaling_Initialize` with startup stage `preloader`. That installs the early graphics hooks (`native/Presentation/GraphicsHooks.cpp`) before Unity creates its device and swap chain, and reads `[Frame Generation]` to choose native D3D11 or the D3D12 bridge. The plugin later calls it again with `plugin-awake`, which only verifies that the hooks saw the device and swap chain.

`[Diagnostics] TraceGraphicsStartup` (default true) controls whether the preloader installs these hooks. DLSS, the bridge and Frame Generation need them; false keeps Frame Generation off and DLSS unavailable (Auto upscaling selects FSR, an explicit DLSS choice reports `UPSCALER_UNAVAILABLE`).

## Automated

`build/Test-Startup.ps1` (part of `build/Test.ps1`) runs `RiskOfRainUpscaling.StartupTests` against a fake `UnityPlayer.dll`. It keeps the fixture's existing resolver and graphics hook, and exercises dynamic device/factory lookup, native and bridge presentation, buffer access, resize and Present, with paths containing spaces and an `Author-Package` folder. Repeated preloader initialization must keep the chain and device count. Config cases cover Frame Generation Off/On, multipliers 2x–6x, out-of-range and invalid values.

Two failure cases cover initialization after graphics creation and a hooked device without a hooked swap chain. Both must log one `WARNING EARLY_HOOKS_MISSED`, report Frame Generation unavailable and stay off after an enable request. Logs are saved as `build/test-results/startup-*.log`.

`RiskOfRainUpscaling.GraphicsHooksTests --warp/--hardware` checks that the hooks pass device creation through.

## Game check

Keep `TraceGraphicsStartup = true`. In `BepInEx/RiskOfRainUpscaling-Diagnostics/native.log`, verify `Initialize stage=preloader`, `BEFORE_D3D11_CREATE_DEVICE` and `SWAPCHAIN_CREATED` before `Initialize stage=plugin-awake`, and `FRAME_BRIDGE_READY` when Frame Generation is enabled at startup. If startup is unavailable, also check `BepInEx/LogOutput.log`. A late plugin load does not install replacement hooks; missed early hooks keep Frame Generation off and DLSS unavailable until restart.

## Old installs

`build/Test-ProfileInstall.ps1` (part of `build/Test.ps1`) runs `build/Install-Profile.ps1` against fixture profiles: installed files must match the package hashes, a previous install of this mod is backed up, and the config and other mods are left unchanged.
