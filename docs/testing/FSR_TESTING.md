# FSR upscaling and Frame Generation

AMD FSR now provides both halves of the pipeline, so the mod runs on non-NVIDIA GPUs:

| Mod Options (`BepInEx/config/xanderfrangos.riskofrainupscaling.cfg`) | Values | Applies |
|---|---|---|
| Upscaler (`[Upscaling] Upscaler`) | **Auto** (DLSS on supported NVIDIA GPUs, otherwise FSR), DLSS, FSR | Live, with a history reset |
| Frame Generation Backend (`[Frame Generation] Backend`) | **Auto** (DLSS Frame Generation on supported NVIDIA GPUs, otherwise FSR), DLSS, FSR | Restart |

Both default to Auto. Auto falls back to FSR only when DLSS never started in this session (for example a GTX GPU or missing runtime). A DLSS failure after it ran still latches reconstruction off until restart, as before. Explicit DLSS on another vendor reports reconstruction or Frame Generation unavailable instead of loading NVIDIA runtimes.

The F6 overlay shows `SR DLSS`/`SR FSR` and `FG FSR 2x enabled`/`FG DLSS 4x enabled`. Mod Options → Risk of Rain Upscaling lists both under Graphics; the related controls are Upscaler Quality and Frame Generation.

## FSR upscaling

The quality levels are unchanged: DLAA maps to FSR Native AA, and Mod Options labels it Native AA when the Upscaler resolves to FSR (the config value stays `DLAA`); Quality, Balanced, Performance and Ultra Performance map to FSR's modes and render at FSR's ratios, which match DLSS except Balanced (1/1.7, 59%, rather than 58%). With Auto, render sizing follows the runtime native code last selected, otherwise the GPU vendor; if Auto on an NVIDIA GPU falls back to FSR, Balanced rebuilds once at FSR's size. `UPSCALER_SIZES` shows FSR's recommended size.

Unity renders with D3D11 and the FidelityFX API is D3D12-only. Each evaluation copies the converted color/depth/motion into shared textures, signals a shared fence, and a private D3D12 queue on the same adapter waits on the GPU, runs FSR and signals back; D3D11 waits on the GPU and copies the result. There is no CPU stall. `FSR_CONTEXT` logs the provider in use and the available versions.

- **Version:** the SDK selects FSR 4 where the GPU and driver support it, otherwise FSR 3.1.5. On the RTX 5080 the choices are 3.1.5 and 2.3.4.
- **HDR placement** (before post-processing): HDR with FSR auto-exposure. **End of effects**: LDR; the sRGB-encoded "perceptual" input is declared to FSR as non-linear.
- **Sharpening:** FSR's sharpening stays off; the existing RCAS pass still applies.
- **Outlines:** FSR has no alpha upscaling, so `TEMPORAL_OUTLINES unavailable` is expected. At lower qualities outlines are drawn at full resolution after reconstruction (`FULL_RESOLUTION_OUTLINES`, with SMAA), whichever Outlines mode is selected; at Native AA they are drawn after reconstruction with SMAA.
- **Frame Generation inputs:** FSR evaluations feed Frame Generation exactly like DLSS ones, so AMD systems now generate frames.

## FSR Frame Generation

The FidelityFX frame-generation swap chain replaces the bridge's display swap chain and paces generated and rendered presents itself. FSR generates one frame per rendered frame: 3x–6x run at 2x. Interpolation runs on the presenting queue inside Present, so the bridge's post-Present fence keeps every input alive. The SDK chose FSR Frame Generation 3.1.6; the ML version needs an RX 9000 GPU and a newer DirectX 12 runtime than the game ships.

**HUD:** the bridge presents the exact pre-HUD snapshot, so FSR never interpolates the HUD, and registers the HUD as a separate premultiplied UI layer that the swap chain composites onto rendered and generated frames. The layer is the final frame minus the HUD-less snapshot scaled by the measured HUD opacity (F6 panel hidden: `FRAME_GEN_UI_LAYER source=measured HUD opacity`), so translucent panels stay see-through. Without a valid opacity capture, every pixel the HUD changed is opaque UI (`source=HUD difference mask (opaque)`): the scene behind translucent panels then updates only on rendered frames. `FRAME_GEN_HUD_OPACITY unsafe reason=...` names what blocked the measured opacity (a screen-space overlay canvas, a HUD image effect or the diagnostic overlay). The bridge counts the mask's opaque pixels and logs `FRAME_GEN_UI_LAYER_COVERAGE`. When the mask stays above half the screen it would paste the rendered frame over every generated one, so `FRAME_GEN_UI_LAYER_COVERING` switches to FSR's HUD-less detection (backbuffer with HUD plus the HUD-less snapshot) until coverage stays below 25% (`FRAME_GEN_UI_LAYER_RESTORED`). The game's HUD camera has an image effect, so it draws into a RenderTexture whose rows are bottom-up on D3D; the HUD-less snapshot and the measured opacity are flipped to swap-chain rows (`FRAME_GEN_HUDLESS_SOURCE ... RenderTexture rows flipped`). Before this, the upside-down snapshot made the mask cover ~99.9% of the screen and fed FSR an upside-down HUD-less image. `PipelineTests --fg --visible [--fsr] [--ui-recomposition] --hud-render-texture` reproduces that layout. The first game test used FSR's HUD-less detection instead, which smeared the HUD during camera motion.

Reflex and PCL are NVIDIA-only: with FSR Frame Generation the game's FPS cap uses the native frame-start guard alone (`FRAME_LIMITER_OPTIONS backend=FSR`). The F6 limiter label reads `Frame-start guard 60 base FPS` (or `Unlimited (no Reflex)`) instead of `Reflex + guard`.

## Game test: NVIDIA PC

1. Build and install. In Mod Options set Upscaler = FSR and Frame Generation Backend = FSR, then restart. Keep Frame Generation enabled, windowed/borderless, VSync off.
2. Launch. `native.log` should show `UPSCALER_SELECTED selected=FSR`, `FSR_CONTEXT`, `FRAME_GEN_BACKEND selected=FSR` and in gameplay `FRAME_GEN_ENABLED ... multiplier=2x`. F6 should read `SR FSR`, `FG FSR 2x enabled` and a `Frame-start guard` limiter while the game FPS cap is set.
3. Cycle quality with F7 and compare DLSS vs FSR (switch `Upscaler` live) for shimmer, ghosting on particles/transparents (FSR has no reactive mask yet), outline appearance, and HDR bloom/brightness.
4. With FSR Frame Generation, compare F11 Off/On during camera motion and HUD-heavy moments, with the F6 panel hidden (measured opacity) and shown (difference mask). The HUD should stay sharp and fixed while the camera turns; check translucent panels and the crosshair.
5. Set the game FPS cap to 30, then 60, then unlimited. **Watch for FSR Frame Generation staying slower than the 60 cap after a period at 30.** The synthetic fixture showed FSR's swap-chain pacing holding about 39 base FPS under a 60 cap after a 30 FPS stretch; the fixture stalls on a GPU readback every frame, so the game may not reproduce it.
6. Resize, alt-tab, change stages and return to the menu. Then set both settings back to Auto and confirm DLSS returns unchanged.

## Game test: Steam Machine (SteamOS/Proton)

This is also the Proton feasibility check.

1. Launch options: `WINEDLLOVERRIDES="winhttp=n,b" %command%`. Leave both settings on Auto and Frame Generation enabled.
2. Confirm in `native.log`: early device/swap-chain observation; `UPSCALER_SELECTED selected=FSR`; `FSR_READY` and `FSR_CONTEXT provider=...` (report the provider: FSR 4 on RDNA 3 depends on the SteamOS/Proton version); `FRAME_GEN_BACKEND selected=FSR`; `FRAME_GEN_ENABLED`; no `FSR_FAILED`, `FRAME_BRIDGE_FAILED` or `FRAME_GEN_FAILED`.
3. Wine may not validate Authenticode signatures; `FFX_SIGNATURE_UNVERIFIED` is then expected, only under Wine. A failure opening shared textures or fences (`FSR shared texture on D3D12`, `FSR shared fence on D3D11`, `FRAME_BRIDGE_FAILED Shared ...`) would mean DXVK/vkd3d-proton interop needs a different approach; send the logs.
4. Play a stage at a 1440p/4K output with Quality and Performance, windowed and fullscreen (gamescope), FG on and off. Report image quality, stutter, black or frozen frames, and the F6 FPS readouts.

Send `native.log`, `managed.log` and `BepInEx/LogOutput.log` for any issue.

## Evidence and limits

`build/Test.ps1 -FrameGeneration` runs the existing suite; it does not run the FSR modes below. Additional fixture modes on the RTX 5080:

- `RiskOfRainUpscaling.PipelineTests --hardware --fsr-upscaler` (and `--bridge`): every upscaler pipeline fixture through FSR: 128 end-of-effects evaluations through both Present APIs, lower-resolution Quality 171x96→256x144 with preserved brightness, HDR evaluation before post-processing with peak 8.0 preserved, alpha preservation, retirement and re-enable, and the documented temporal-outline fallback.
- `--fg --visible --fsr` (with `--fg-multiplier 4`, `--fg-live`, `--reflex-limit`): FSR Frame Generation at 2x (254 presents per 128 frames), clamping, live requests and the cap guard. `--fg --visible --fsr --fsr-upscaler` runs the complete AMD path. Both read back the UI layer (`FG_UI_LAYER_VERIFIED ... difference mask`); `--fg --visible --fsr --ui-recomposition` draws the HUD through the opacity capture (`... measured opacity`).

During M3 the DLSS Reflex limiter fixture (`--fg --visible --reflex-limit`) failed intermittently for about ten minutes, with the driver enforcing the cap at the presented rate (base FPS halved). The committed pre-FSR build and the current build each passed every run before and after that window, including immediately after FSR runs, so this is attributed to transient driver/system state, not the FSR changes. Watch for it.

Not yet verified: game image quality with FSR, behavior under Proton and gamescope, FSR 4 selection on AMD hardware, and the cost of the D3D11/D3D12 round trip in game. Not implemented: an FSR reactive/transparency mask (M5).
