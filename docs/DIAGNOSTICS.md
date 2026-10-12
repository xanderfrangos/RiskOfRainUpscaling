# Diagnostics

Everything here is optional: upscaling and Frame Generation do not depend on it, and a diagnostics failure only turns diagnostics off for that camera.

## Logs

`[Diagnostics] Logging Level` (Off, Error, Warning, Info, Debug; default Off) filters all mod output live and at startup. Each level includes the more severe ones.

Files are in `BepInEx/RiskOfRainUpscaling-Diagnostics/`:

| File | Contents |
| --- | --- |
| `managed.log` | Plugin messages with timestamp and level: startup environment, pipeline attach/detach and fallbacks, upscaler session, limiters. Also in BepInEx's `LogOutput.log`. |
| `native.log` | Native DLL: graphics hooks, bridge, scene inputs, upscaler, Frame Generation, Streamline/FFX messages forwarded by level. |
| `FrameGeneration/sl.log` | NVIDIA Streamline's own Frame Generation log (separate from the level setting). |
| `captures/<UTC time>/` | Capture Diagnostics output (below). |

Useful tags:

| Area | Tags |
| --- | --- |
| Startup | `Initialize stage=preloader` / `plugin-awake`, `BEFORE_D3D11_CREATE_DEVICE`, `SWAPCHAIN_CREATED`, `EARLY_HOOKS_MISSED`, `FRAME_BRIDGE_STARTUP`, `FRAME_BRIDGE_READY` |
| Pipeline (managed) | `PIPELINE_READY`, `PIPELINE_ATTACHED`, `PIPELINE_DETACHED`, `UPSCALING_FALLBACK`, `UPSCALING_RETRY`, `UPSCALER_QUALITY`, `DISPLAY_POST_PROCESSING` |
| Upscaler | `UPSCALER_SELECTED`, `UPSCALER_SIZES`, `UPSCALER_READY`, `UPSCALER_EVALUATED`, `UPSCALER_UNAVAILABLE`, `UPSCALER_EVENT_FAILED`, `HISTORY_RESET`, `HISTORY_SUMMARY`, `DLSS_*`, `FSR_*`, `STREAMLINE_PRESENT_*` |
| Sharpening and outlines | `RCAS_STRENGTH`, `RCAS_CHANGED`, `OUTLINES`, `OUTLINE_MASKS`, `TEMPORAL_OUTLINES`, `FULL_RESOLUTION_OUTLINES` |
| Frame Generation | `FRAME_GEN_ENABLED`, `FRAME_GEN_PRESENT` (actual SDK frames), `FRAME_GEN_MULTIPLIER` (`requested=Dynamic selected=Dynamic target=...`, or the fixed fallback and why), `FRAME_GEN_CAPABILITIES` (`maxGenerated`, `dynamic=` support), `FRAME_GEN_DYNAMIC_FALLBACK` (the SDK refused Dynamic; fixed mode for the session), `FRAME_GEN_UI_RECOMPOSITION`, `FRAME_GEN_UI_LAYER`, `FRAME_GEN_HUD_OPACITY` (why measured HUD opacity is unavailable), `FRAME_GEN_UI_LAYER_COVERAGE`/`_COVERING`/`_RESTORED` (FSR difference-mask share of the screen), `FRAME_GEN_UI_LAYER_DIFF`/`_DUMP` (Debug only, once per session when the mask covers the screen: a difference histogram and `ui-layer-dump/final.bmp`, `hudless.bmp`, `difference-x32.bmp`), `FRAME_GEN_HUDLESS_SOURCE` (producer or a bound HUD RenderTexture, whose rows are flipped), `FRAME_GEN_DISPLAY_MAPPING` (game resolution, window, and the rectangle Frame Generation runs on when Unity scales the game), `FRAME_GEN_GAME_BUFFER` (Unity's game-sized screen buffer and its probed row order), `FRAME_BRIDGE_SOURCE_SIZE`, `FRAME_GEN_FAILED` |
| Pacing | `REFLEX_LIMITER_*`, `VSYNC_FPS_GUARD`, `REFLEX_PACING` (`effectiveMultiplier`: SDK presents per rendered frame, the ratio Dynamic chose), `DYNAMIC_FG_TARGET` (`fps_max` sent as Dynamic's output target) |
| Diagnostics | `DIAGNOSTICS_CREATED`, `DIAGNOSTICS_FAILED`, `CAPTURE_*` |

Debug adds: the camera inventory (every 2 s, only at Debug), `CAMERA_ORDER`/`SCENE_TAIL` traces for the first frames after each attach, `CAMERA_COMMANDS`/`POST_PROCESS` (command buffers and image effects on each camera, checked every 5 s only at Debug), `HISTORY_FOV_CONTINUITY`, `MIP_BIAS`, `PARTICLE_MOTION`, `REFLEX_PACING`, and sampled native `SCENE_INPUTS_SOURCE`/`UPSCALER_EVALUATED` lines. Use Debug when collecting rendering evidence; Info is enough for normal bug reports.

## Debug overlay

**Toggle Debug Overlay** (default F6, `[Diagnostics] ShowOverlay`) shows a status line and preview panels:

- Status: presentation runtime (`Native D3D11` / `D3D12 bridge`), upscaler (`SR DLSS`/`SR FSR`), Frame Generation state and multiplier, limiter, SDK presents.
- Panels: scene color (render size), depth (log distance), motion (UV × 20), the scene before the HUD, and the final image after the HUD.

The previews and final copy are drawn only while the overlay is visible. While shown, the overlay disables UI recomposition for Frame Generation, because IMGUI draws after the HUD replay; close it when judging Frame Generation.

## Capture Diagnostics

**Capture Diagnostics** (default F8) saves one frame to `captures/<UTC time>/`:

| File | Contents |
| --- | --- |
| `final.png` | The HUD camera's image at its `AfterEverything`: world and HUD, without Screen Space Overlay canvases or IMGUI. With the D3D12 bridge it is the D3D11 producer image, not the physical display buffer. |
| `scene-color.png` | Render-sized scene color at the end of the effect chain (linear values written as PNG; see `scenePngEncoding`). |
| `scene-before-hud.png` | Display-sized scene before the HUD (the upscaler output when upscaling). |
| `depth-preview.png`, `motion-preview.png` | The overlay's depth and motion previews. |
| `capture.json` | Plugin version, scene, sizes, quality, upscaler, presentation mode, camera matrices, depth range, motion maximum/non-finite counts and where each image was taken. |

All images come from the same Unity frame; textures are allocated for the capture and released afterwards. `[Diagnostics] CaptureWhileOff` keeps scene inputs available for the overlay and captures while upscaling is Off, for a native-resolution comparison.

## Cost profiling

`[Diagnostics] Profiling = true`, or **Cost Profiling** in Mod Options (applies live, independent of the logging level), writes `profile.log` every 2 seconds. Each block starts with the configuration (upscaler, quality, render and output size, Frame Generation, frame limit); a configuration change starts a new block, so switch Off → DLAA → Quality and compare blocks.

- **GPU (D3D11 timestamps)**: rows in pipeline order. `[scene:A -> scene:B]` rows are whole Unity camera stages between two camera events on the scene, skybox and HUD cameras, including the game's own work. They are recorded with upscaling Off too, so stages such as motion vectors or display-sized post-processing can be compared. Indented rows are the mod's own steps inside a stage: depth capture, motion copy, colour/depth/motion conversion, the DLSS/FSR feature itself, alpha merge, RCAS, outline passes, tail copies, the HUD-less copy and the bridge's D3D11 copies. `seen` is the share of frames in which a step ran. `cpu avg` is the render-thread time of native steps.
- **Mod-owned GPU work** sums the mod's top-level steps per frame. The `GPU span` is wall time from the first timed event to Present, so it includes idle gaps.
- **D3D12 presentation queue** (bridge only): the bridge's copy list, and the presenting-queue span over Present, which includes the Frame Generation runtime's GPU work and any idle time on that queue.
- **CPU**: bridge Present time, waits for the previous D3D12 frame and for FG input release, SDK preparation and the D3D12 Present call.

FSR's own D3D12 work and the HUD opacity replay cannot be timed separately; the HUD row with Frame Generation on includes the game's HUD draws. `build\native-nmake\RiskOfRainUpscaling.PipelineTests.exe --hardware --bridge --profile` writes a fixture profile to `build/test-results/profile.log`.

## Developer tests

`build/Test.ps1` runs the managed tests and the native fixtures (scene inputs, diagnostics, upscaler pipeline on WARP and hardware, frame bridge, HUD opacity, RCAS, pacing, logging, startup hooks). `-FrameGeneration` and `-ReflexLimiter` add the interactive hardware Frame Generation runs, which need a focused desktop.
