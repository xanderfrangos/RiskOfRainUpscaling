# UI recomposition

The mod supplies same-frame HUD opacity to DLSS Frame Generation and requests
`enableUserInterfaceRecomposition`. This lets DLSS interpolate scene and UI
separately. It does not select or report NVIDIA App's Frame Generation Preset B.
The public Streamline 2.14.1 interface exposes the recomposition option rather
than an FG preset selector.

## Using it

UI recomposition is always on with DLSS Frame Generation; there is no setting.
Hide the debug overlay (**F6**): it draws later than the captured HUD, so while
it is shown Frame Generation uses ordinary HUD-less generation.

`FRAME_GEN_UI_RECOMPOSITION ... enabled=1` in native.log confirms the requested
path. This log reports our SDK request, not which NVIDIA model actually ran.
Select Preset B in NVIDIA App for supported profiles and verify the active
model through NVIDIA's DLSS statistics if the override is available for the mod.

## Capture and fallback

The native companion mirrors the HUD's actual draw calls into an owned texture,
preserving shader opacity, mesh coverage, scissor and stencil clipping. It
accumulates opacity as `a + (1-a) * previous`, independently of the game's alpha
blend settings, then extracts a display-sized R8 mask. No extra camera render,
black/white rerender, or scene/final color difference supplies production alpha.
The original game draw, RGBA result and depth/stencil writes remain unchanged.
The R8 mask follows the same per-slot D3D11/D3D12 sharing and NVIDIA consumption
fences as the existing inputs; only D3D12-owned snapshots are tagged.

Straight/premultiplied alpha-over HUD materials are supported. Additive/opaque
or custom blends, partial RGB masks, multiple/offscreen targets, predication,
shader UAV/stream-output writes, GPU queries, deferred HUD commands, missing
completion or a changed context dispatch table invalidate the mask. The current
frame then uses the existing HUDless FG path and clears the UI tag. Resource or
sharing failures also retain ordinary FG. No stale mask is intentionally reused.

The managed guard also excludes active screen-space overlay canvases, the F6
preview panel and unknown HUD image effects. The game's DamageIndicator is
allowed only in its verified zero-intensity branch. Its nonzero intensity can
persist after a hit, so UI recomposition may remain off afterward; broader
damage-effect coverage requires a separate capture implementation. These guards
prioritize correct UI inputs over claiming complete UI coverage. Other mods or
IMGUI overlays outside the camera pass still need game validation.

There is an additional draw for each supported HUD batch, native state queries,
an alpha extraction pass and mask copies. Measure base FPS and memory to judge the
capture cost.

## Validation

Run `build/Build.ps1 -UseCachedReferences`, then `build/Test.ps1 -FrameGeneration`.
HUD opacity fixtures run on WARP and the hardware adapter and verify overlapping
transparency, clipping/stencil, premultiplied UI, identical game RGBA/state,
unsupported blends, empty HUD and missing/mismatched completion.
The hardware FG UI fixture verifies the D3D12 R8 mask against an actual HUD draw,
including transparent scene pixels, and requests recomposition during real FG.
The remaining suite covers ordinary FG fallback, frame modes, resize and Off/on.

The interactive hardware UI fixture passed: native R8 readbacks matched the
drawn HUD; NVIDIA's log reported `Preset B selected, enabling UIR` and
`UserInterfaceRecompositionEnabled: (true)` with the existing preset override.
128 rendered frames produced 255 SDK presentations (2x), with 287 presentations
across the complete lifecycle fixture. WARP and hardware opacity tests also
passed. The full `build/Test.ps1 -FrameGeneration` suite passed, including the
new UI fixture, all 2x-6x modes, live multiplier changes, application wait-handle
mode, ordinary fallback, resizing and recreation. Evidence is in
`build/test-results/regression-ui-recomposition.log`, `frame-generation-ui.log`
and `frame-generation-ui-sl.log`. The build passed with no warnings or errors.
This confirms the synthetic path; the mod does not change NVIDIA App
settings or guarantee that every game/profile selects the same model.

In-game testing: compare text, crosshairs, health bars, translucent
panels and menus with Frame Generation On/Off; test damage indicators, F6 overlay,
resize, alt-tab, F11 Off/on and multipliers. Confirm fallback logs where expected
and compare at the same base FPS. Synthetic fixtures do not prove visual quality
or preset availability in the actual Risk of Rain 2 process.

References: [NVIDIA UI inputs](https://github.com/NVIDIA-RTX/Streamline/blob/v2.14.1/docs/ProgrammingGuideDLSS_G.md#51-tagging-resources),
[UI recomposition](https://github.com/NVIDIA-RTX/Streamline/blob/v2.14.1/docs/ProgrammingGuideDLSS_G.md#66-enabling-user-interface-recomposition),
[SDK option](https://github.com/NVIDIA-RTX/Streamline/blob/v2.14.1/include/sl_dlss_g.h).
