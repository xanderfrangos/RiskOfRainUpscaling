# Upscaling: behavior and testing

Covers DLAA/DLSS quality levels, placement before post-processing, interactable outlines, RCAS sharpening, history resets and main-menu support. FSR-specific notes are in [FSR testing](FSR_TESTING.md); diagnostics in [DIAGNOSTICS.md](../DIAGNOSTICS.md).

## Settings

All in `BepInEx/config/xanderfrangos.riskofrainupscaling.cfg`, `[Upscaling]` (Mod Options → Risk of Rain Upscaling):

| Key | Default | Notes |
| --- | --- | --- |
| `Enabled` | true | **Toggle Upscaling** (F10) toggles it with the last quality. |
| `Quality` | DLAA | DLAA (100%), Quality (67%), Balanced (58%; 59% with FSR), Performance (50%), Ultra Performance (33%). **Cycle Upscaler Quality** (F7). Applies next frame with a history reset. |
| `Upscaler` | Auto | Auto, DLSS, FSR. Live. |
| `DLSS Preset` | Default | Default (NVIDIA per mode: K for DLAA/Quality/Balanced, M Performance, L Ultra Performance), K, M, L, J (transformer) or E (CNN, cheapest). DLSS only. Applies next frame with a history reset. |
| `Sharpening` | 0.7 | RCAS strength 0–1; 0 disables. Live, without a history reset. |
| `MipBiasOffset` | −1 | −2 to 0, added to log2(render/display) below 100%. Raise toward 0 if fine textures shimmer. |
| `MainMenu` | true | Upscale (and allow Frame Generation on) the intro cutscene, title/main-menu and character select scene cameras. |

## How it works

**Placement.** The upscaler evaluates the HDR scene after transparents, before the post-processing stack (bloom, tonemapping, vignette, grain, the damage overlay). Screen-anchored effects therefore never enter upscaler history. Input is sanitized (NaN/Inf removed, clamped to 0–65504) with auto exposure. RCAS runs later, after post-processing and before the HUD. A scene camera without an active post-processing layer, or one with a scene image effect that cannot run at display size (logged), uses End of Effects placement instead: the tonemapped image, sRGB-encoded for the upscaler's LDR mode.

**DLAA (100%)** writes the result back into the scene target in place with its original alpha; only fully evaluated frames are written, and a failure leaves the scene untouched and latches upscaling Off.

**Lower resolutions.** The world renders at the render size. The upscaler writes a display-sized output, and Unity's post-processing (PPv2) stack is rebuilt with a display-sized context that reads it. `OutlineHighlight` and `BlurOptimized` become plain blits in the render-sized `OnRenderImage` chain and rerun at display size from the mod's scene tail. Frames the upscaler does not evaluate (first frames, second renders) get a bilinear, NaN-free upscale. The upscaled world is written into the HUD camera's own target at `BeforeForwardOpaque`, before any HUD element (the HUD camera's `DamageIndicator` effect makes it draw into an intermediate texture, so writing to the display directly would be overwritten). World-anchored UI (health bars, pings, damage numbers, highlights, tooltips) is corrected by Harmony patches that scale scene-camera screen points (`DISPLAY_SPACE_PATCHED`). Jitter uses render-pixel offsets with 8 × (display/render)² phases. Mipmapped textures get the LOD bias above; originals are restored on Off or detach, and newly loaded textures are picked up every 10 s.

**Interactable outlines.** With upscaling before post-processing, the game's `OutlineHighlight` would draw after reconstruction: nothing anti-aliases it and the camera jitter moves it every frame.

- *Temporal outlines* (DLSS with alpha upscaling): before evaluation, the mod records the game's fill pass for every highlighted renderer into a render-sized mask with the jittered projection, and the game's `SobelBuffer` material into an R8 edge texture. Native code writes the outline weight `(mask.r > 0) × saturate(0.0275 + saturate(sobel × 41 − 1))` into the alpha of its copy of the DLSS input; DLSS reconstructs it; the composite event blends the strongest nearby highlight colour back in at display size at `OutlineHighlight`'s place in the chain. Unevaluated frames composite the current render-sized weight.
- *Full resolution* (lower qualities with `[Upscaling] Outlines = FullResolution`, the default, or without DLSS alpha upscaling): the mod draws the game's outline itself after reconstruction, entirely at display size. The game's fill pass discards fragments more than 0.001 units behind `_CameraDepthTexture` at their screen UV, and its `SobelBuffer` pass compares `_MainTex` depths 0.002 UV apart (both decoded from the game's shader bytecode). So native code first writes a display-sized occlusion depth (the farthest of the 3×3 nearest render texels, which the unjittered fill fragment never fails). The highlighted renderers are drawn unjittered into a display-sized depth texture through each material's `ShadowCaster` pass (how Unity builds camera depth), with `unity_LightShadowBias` zeroed. The game's fill pass then draws the mask with `_CameraDepthTexture` bound to the occlusion depth. Native code builds a display-sized Sobel depth: the object depth inside the mask; where a material has no `ShadowCaster` pass, the nearest surface among the 3×3 render texels; outside the mask the farthest, filtered bilinearly within 2% of that surface, so depth steps exactly on the display-sized mask edge. It then runs the game's Sobel filter (eight point-sampled `Linear01Depth` taps 0.002 × (size + 1) pixels away, counting only farther depth relative to the centre, unweighted, centre clamped to 0.001) at the standard 8× MSAA sub-pixel positions and averages the composite's weight `saturate(sobel × 41 − 1)`. The game's `SobelBuffer` draws that weight with no anti-aliasing, so lines snap on or off per pixel even at native resolution. The average is stored as `(weight + 1) / 41` in a half-float texture that replaces `_SobelTex` for the game's composite pass, which decodes it exactly. SMAA High follows, with edges taken from the outline alone. An occluder in front of a highlighted object hides it from one render texel inside the occluder's edge. Without the game's Sobel effect only the tint is drawn, as with temporal outlines.
- *Otherwise* (DLAA without alpha upscaling, or if the full-resolution textures are unavailable): the mod draws the game's outline after reconstruction with the unjittered projection, at render size (so its depth test matches `_CameraDepthTexture`), then SMAA High. At lower qualities the mask is upscaled natively with one-display-pixel edges.
- If the game's outline members change in an update, the game's own drawing is kept (`OUTLINES` warning). At End of Effects outlines are drawn before the upscaler, which anti-aliases them.

**RCAS** is AMD's 32-bit RCAS with noise suppression (FidelityFX-FSR `a21ffb8f6c13233ba336352bdff293894c706575`, MIT license in the package). The wrapper converts linear SDR to/from gamma 2, clamps neighbor reads at edges, and keeps original alpha and out-of-range values. Strength is the filter's linear attenuation coefficient.

**History.** Resets happen on the first frame, skipped frames, gaps over 0.5 s, position jumps over 5 units, rotations over 45°, and FOV steps greater than both 5° and 180°/s (`max(5, elapsedSeconds × 180)`). Gradual sprint/zoom FOV changes keep history, the real previous projection and the jitter sequence. `HISTORY_RESET` logs each reset with its reasons.

**Main menu.** The `title` scene's registered scene camera is accepted without a player body when the title scene, an active menu controller in that scene, no active run, a full viewport and exactly one eligible rig are present. Character select (the `lobby` scene) is accepted the same way when the lobby's `PreGameController` is active in the camera's scene and no run exists. The intro cutscene (the `intro` scene) is accepted the same way when its `IntroCutsceneController` is active in the camera's scene; its subtitles, letterbox and fade are a screen-space overlay canvas, so Frame Generation uses the opaque difference mask there (`FRAME_GEN_HUD_OPACITY unsafe reason=screen-space overlay canvas Canvas`). Menu UI renders afterward. Splash, loading, spectator and preview cameras stay excluded.

## Automated coverage

`build/Test.ps1` runs `RiskOfRainUpscaling.PipelineTests --warp` and `--hardware`:

- DLAA in place: on WARP the scene stays bit-exact; on hardware 32 HDR evaluations keep values far above 1, NaN/Inf come out finite, alpha and Unity bindings are preserved.
- Quality 171×96 → 256×144: finite HDR display-sized output with the nearest render pixel's alpha; the scene is never written; the fallback event fills the output without touching history.
- RCAS at the tail copies exactly at 0 and sharpens with alpha intact at 0.7; `RcasTests` compares the shader against a CPU reference at 0, 0.2 and 1, including edges, odd sizes and out-of-range colors.
- Temporal outlines: registration validation, reconstructed border weight 1.00, interior within 0.03 of the tint, outside within 0.02 of 0, alpha untouched; outline mask upscale gives 146 partial edge pixels against 728 bilinear.
- Full-resolution outlines (171x96 → 256x144, also on WARP and with FSR): registration rejects swapped, wrongly typed, wrongly sized and invalid arguments; unregistered events write nothing; the occlusion depth is exactly the farthest of the 3×3 render texels and never nearer than the pixel's own texel. One object exists only in the display-sized object depth and one only in the render depth (no `ShadowCaster` pass): both get Sobel edges whose decoded weight is exactly the share of the eight sub-pixel positions with a tap outside the object (268 anti-aliased edge pixels each), zero inside and nothing outside the mask.
- Managed tests: render sizes for every quality, FSR ratios, jitter phase counts, mip bias, quality cycling, placement policy, animated FOV at 15/30/60/120 FPS, cut and gap protections, and all 512 menu-eligibility combinations.

The game-only parts (display-sized PPv2 rebuild, the outline/blur rerun, recorded highlight and Sobel draws, the `_CameraDepthTexture`/`_SobelTex` substitutions, SMAA, motion stability) have no automated test.

## Game test

1. Start a run at **DLAA**. native.log: `UPSCALER_READY ... placement=before-post-processing ... write=scene in place` and `RCAS_APPLIED`. managed.log: `PIPELINE_ATTACHED ... quality=DLAA`.
2. Pan and move: screen edges, vignette, the damage overlay, bloom on bright projectiles, grain and overall exposure should be stable, with no trails on screen-anchored effects.
3. Press **F7** through Quality, Balanced, Performance and Ultra Performance. For each:
   - managed.log `PIPELINE_ATTACHED ... world=<render> output=<display>`, native.log `UPSCALER_READY ... write=display-sized output` and `DISPLAY_POST_PROCESSING display-sized ...`.
   - The world stays live while moving (a frozen world was an old failure); translucent HUD panels do not darken over time; health bars, pings and highlights sit on their targets.
   - Bloom radius, grain size, vignette and the damage overlay look like DLAA rather than blocky; textures do not shimmer (else raise `MipBiasOffset`).
   - Interactable outlines (chests, shrines, teleporter) sit on their objects, the right way up, with no stripes. At lower qualities with Outlines = Full Resolution (and always with FSR), the outline lines hold still, show no render-pixel stair steps (also along a chest's lid seam), and their edges are soft over about a pixel rather than speckled; occluders in front of a highlighted object cut it cleanly. `managed.log` shows `OUTLINES drawn by RiskOfRainUpscaling: ... full resolution (...; objects=N depthDraws=M sobel=8x anti-aliased)`; `depthDraws=0` with objects means no highlighted material had a `ShadowCaster` pass and `native.log` `FULL_RESOLUTION_OUTLINES`. With Outlines = Temporal and DLSS they hold still and look as smooth as the scene (`OUTLINES temporal: ...`).
   - The pause/menu blur still appears.
4. Change **Sharpening** between 0, 0.2 and 1 on the same scene: fine edges and foliage change, HUD/menu text does not, no history reset (`RCAS_CHANGED`).
5. Sprint and zoom repeatedly at a 30 FPS cap: no trails or distortion during FOV changes; `HISTORY_SUMMARY` at detach reports retained FOV frames, and Debug shows `HISTORY_FOV_CONTINUITY` with a non-identity previous mapping.
6. Return to the title menu: managed.log `PIPELINE_ATTACHED role=main-menu-scene sceneName=title`; the background animation and edges look right; F10 and F7 work; open character select: `PIPELINE_ATTACHED role=character-select-scene sceneName=lobby`; survivor models and the background are reconstructed, the character select UI stays sharp and in place, and Frame Generation runs; set `MainMenu = false` and relaunch to confirm gameplay-only. For the intro, set `intro_skip 0` in the console and relaunch: managed.log `PIPELINE_ATTACHED role=intro-cutscene sceneName=intro`; the ship interior is reconstructed with no ghosting across camera cuts (`HISTORY_RESET` at each cut), subtitles and the letterbox stay sharp and in place, and Frame Generation runs; the pipeline reattaches as `main-menu-scene` when the title loads.
7. Return to DLAA with F7 and Off with F10: the original image, anti-aliasing and texture sharpness return.
8. Watch for `UPSCALER_EVENT_FAILED`, `UPSCALER_FALLBACK_FAILED`, `UPSCALING_FALLBACK`, `cannot run at display size`, `Display-sized post-processing setup failed`, `OUTLINES_FAILED`, `TEMPORAL_OUTLINES_FAILED` and `FULL_RESOLUTION_OUTLINES_FAILED`.

## Known limits

- Opaque-only effects (ambient occlusion, SSR, fog, scene tint, Sobel outlines) run at render size before the upscaler.
- The mip bias skips sprite and RawImage textures, which the HUD draws at display resolution. A texture used by both the HUD and the world keeps its original bias in the world too, and a RawImage texture first shown after a scan stays biased until the next scan (up to 10 seconds).
- Another mod's camera-projection UI is not covered by the display-space patches; another mod's scene image effect moves upscaling to End of Effects.
- The FOV-cut thresholds are a heuristic informed by logs and need visual confirmation.
