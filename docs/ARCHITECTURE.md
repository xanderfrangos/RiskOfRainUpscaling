# Architecture

Risk of Rain 2 runs Unity 2021.3 on D3D11 with the built-in deferred renderer. The mod is a BepInEx plugin (C#) with a native companion DLL (C++). The plugin decides *what* to render and when; the native DLL does the GPU work on Unity's render thread through `CommandBuffer.IssuePluginEventAndData` callbacks, and owns the vendor SDKs.

```
BepInEx preloader ──► RiskOfRainUpscaling.Native.dll  (early graphics hooks, presentation bridge)
BepInEx plugin    ──► RenderPipeline ──► native render events (scene inputs, upscaler, Frame Generation, diagnostics)
```

## Source layout

### Managed (`src/`)

| Folder | Contents |
| --- | --- |
| `RiskOfRainUpscaling.Preloader/` | Loads the native DLL and installs the graphics hooks before Unity creates its device (`Initialize(..., "preloader")`). |
| `RiskOfRainUpscaling/Plugin.cs` | BepInEx entry point: settings, native load, live setting changes, camera eligibility, the restart prompt. |
| `Pipeline/` | `RenderPipeline` picks the single eligible scene camera, attaches command buffers and falls back safely. `PipelineResources` owns the cameras' render targets and the native scene-input context; `Route()` composes a lower-resolution world into the HUD camera's target. `SceneTail` is the last `OnRenderImage` on the scene camera. `DisplaySpace` patches world-anchored UI while routing. |
| `Upscaling/` | `UpscalerSession` (partial): jitter, history resets, the native upscaler context, Unity post-processing (PPv2) hooks; `.SceneFixups` (mip bias, particle motion), `.DisplayPostProcessing` (post stack at display size), `.Outlines` (interactable outlines). `DlssPolicy`, `DlaaHistoryPolicy`. |
| `FrameGeneration/` | Unity player-loop markers (`FrameGenerationTiming`) and the frame limiters (Reflex, VSync guard). |
| `Settings/` | BepInEx config bindings and optional Risk of Options menu. |
| `Diagnostics/` | `ModLog`, the managed.log startup report, the debug overlay and capture (partials of `Plugin`, `RenderPipeline`, `PipelineResources`). |
| `Shared/` | Files compiled into both plugin and preloader: `NativeApi` bindings, logging and Frame Generation settings, package paths. |

### Native (`native/`)

| Folder | Contents |
| --- | --- |
| `Core/` | `Native.h` (shared includes, `Log`), module init and logging, `CameraFrame`, `FullscreenPass` and `D3D11StateScope` (native draws on Unity's context), `Fullscreen` vertex shader, FidelityFX runtime loading. |
| `Presentation/` | `GraphicsHooks`: device/factory/swap-chain creation and Present/GetBuffer/Resize hooks installed by the preloader. `StreamlinePresentation`: routes D3D11 Present through the Streamline proxy so DLSS sees presentation. |
| `SceneInputs/` | Same-frame raw depth (copied from the scene DSV), motion validation and the scene color after transparents. |
| `Upscaling/` | `UpscalerContext` (create/submit/evaluate/fallback), `Sharpening` (RCAS), `Outlines`, backend selection and the DLSS and FSR backends. |
| `FrameGeneration/` | `FrameBridge` (D3D11 swap-chain proxy presenting through D3D12), Frame Generation front end and DLSS/FSR backends, HUD opacity capture and UI layer. |
| `Pacing/` | Frame-start limiter (`FramePacerWait`). |
| `Diagnostics/` | Optional previews, final-image copy and capture. Its own context: a failure never affects upscaling. |

Shaders sit beside their feature and compile to `build/native-nmake/shaders/<Entry>.h`.

### ABI conventions

- The managed ABI is `extern "C"` exports named `RiskOfRainUpscaling_<Feature><Action>` (`SceneInputsCreate`, `UpscalerSubmitFrame`, `FrameGenerationSetMultiplier`, ...). `NativeApi.Load` binds every export and fails on a missing one; a managed test loads the built DLL to check this.
- Structs passed across the boundary start with `size, version` and have size checks on both sides (`tests/managed/Program.cs`, native `static_assert`s).
- Native test hooks are C++ `__declspec(dllexport)` functions (`SceneInputsReady`, `FrameBridgeReadback`, ...), used only by `tests/native`.

## Frame timeline

For the scene camera, then the HUD camera, each frame:

| When | Who | What |
| --- | --- | --- |
| `Camera.onPreCull` (scene) | `RenderPipeline.BeforeCull` | Records the Unity frame number, the geometry event and diagnostics (only when wanted); `UpscalerSession.Prepare` jitters the projection and submits the `CameraFrame`. |
| `AfterGBuffer` | scene inputs | Validates the G-buffer and copies same-frame depth. |
| `AfterForwardAlpha` | scene inputs, upscaler | Depth again if still bound, the scene color after transparents and motion vectors; then, before post-processing, the upscaler evaluates (DLAA in place; DLSS into a display-sized output). |
| Post-processing | PPv2 | For lower-resolution DLSS, rebuilt at display size reading the upscaler output. |
| `OnRenderImage` tail | `SceneTail` | RCAS (or end-of-effects evaluation), copy of the scene before the HUD. |
| HUD `BeforeForwardOpaque` | routing, Frame Generation | Lower-resolution world written into the HUD camera's target; Frame Generation's pre-HUD snapshot. |
| HUD draws | `HudOpacity` | HUD draw calls replayed into an opacity mask for UI recomposition. |
| HUD `AfterEverything` | diagnostics | Final-image copy and capture freeze, only while the overlay is shown or a capture runs. |
| Present | `GraphicsHooks`, `FrameBridge` | Native D3D11 Present through Streamline, or the bridge: shared slot to D3D12, Frame Generation, D3D12 Present. |

The presentation runtime (native D3D11 or the D3D12 bridge) is chosen once, at startup, from `[Frame Generation] Enabled`, because the swap chain must be replaced before Unity creates it.

## Background research

### Open Shaders

The bridge design follows `alandtse/open-shaders` (a Skyrim SKSE plugin, reviewed at commit `0d169a2a1844130d146d04cf275430518408ef90`, source inspection only):

- It intercepts `D3D11CreateDeviceAndSwapChain` through the game's import table, creates the game-facing D3D11 device and returns a replacement swap chain.
- A separate D3D12 device on the same adapter, upgraded for Streamline, owns a real swap chain created from a command queue.
- `GetBuffer` returns a D3D11 texture with a shared handle; a shared fence orders D3D11 writes, D3D12 copies and presentation, and reuse.
- Separate runtime directories give D3D11 (upscaling) and D3D12 (Frame Generation) independent Streamline instances, both with manual hooking.
- Depth, motion, HUD-free color and UI alpha are tagged per frame; Reflex runs before rendering and PCL markers track the frame; the presenter checks SDK state and actual frames presented rather than an enabled flag.

Its proxy only declares `IDXGISwapChain` and handles buffer zero; Unity needs more (below), so the interfaces were implemented from Unity's observed use rather than copied.

### Unity's swap-chain contract

Observed in game: a two-buffer, 2560×1440, flip-sequential RGBA8 UNORM chain. Unity queries the base swap chain and `IDXGISwapChain2`, and requests buffer zero as both `ID3D11Resource` and `ID3D11Texture2D`. Resize flags `0x842` require preserving mode-switch allowance, the frame-latency waitable object and tearing support; the first Present uses interval 0 with the tearing flag `0x200`. The bridge implements interfaces through `IDXGISwapChain2` (and `3` for buffer index/resize), keeps device ownership and frame-latency semantics, and supports resize to two or three physical buffers. Exclusive fullscreen, HDR and dirty/scroll regions are out of scope.
