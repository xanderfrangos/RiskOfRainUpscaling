# Frame Generation and pacing: behavior and testing

Covers the D3D12 presentation bridge, DLSS Frame Generation (2x–6x), the Reflex limiter and the VSync guard. FSR Frame Generation is in [FSR testing](FSR_TESTING.md), HUD handling in [UI recomposition](UI_RECOMPOSITION_TESTING.md), startup hooks in [startup testing](STARTUP_TESTING.md).

## Settings

`[Frame Generation]` in `BepInEx/config/xanderfrangos.riskofrainupscaling.cfg`:

| Key | Default | Notes |
| --- | --- | --- |
| `Enabled` | false | Read by the preloader before graphics startup: true starts the D3D12 bridge, false native D3D11. Off/on applies live while the bridge runs; enabling from D3D11 prompts for a restart. Live Off keeps the bridge for the session. **Toggle Frame Generation** (F11, when hotkeys are enabled). |
| `Multiplier` | 2 | 2–6 total frames per rendered frame. Live in D3D12. Unsupported values use the GPU/driver maximum (`numFramesToGenerateMax`), reported in the overlay and `FRAME_GEN_MULTIPLIER`. |
| `Backend` | Auto | Auto, DLSS, FSR. Restart. |
| `ReflexFrameLimiter` | true | Replace the game's `fps_max` limiter with frame-start Reflex pacing plus a native interval guard while upscaling is on. |

Generation requires upscaling, windowed/borderless presentation, hardware-accelerated GPU scheduling, same-frame scene inputs, and eligible focused gameplay or the title menu. It suspends (`FRAME_GEN_SUSPENDED`) during menus, loading, pause, resize and missing inputs.

## How it works

**Bridge.** Unity keeps rendering D3D11. The preloader's hooks return a D3D11-facing swap-chain proxy whose buffers are shared textures. Each Present copies the frame into a shared slot; a private D3D12 device on the same adapter presents it, with separate producer/consumer fences protecting slots. A separate D3D12 Streamline instance owns DLSS Frame Generation, Reflex and PCL; the D3D11 instance owns DLSS upscaling.

**Inputs.** After each upscaler evaluation, depth and motion are flipped into swap-chain row order and shared at render size; the pre-HUD scene color is snapshotted at display size from the HUD camera's actual target at `BeforeForwardOpaque`; with UI recomposition, the HUD opacity mask is shared too. Camera constants use the matching flipped clip space. Unity player-loop markers carry the same frame identity to the presenting thread.

**Evidence.** `FRAME_GEN_ENABLED` means generation was requested. `FRAME_GEN_PRESENT ... sdkFrames=N` and the cumulative SDK count prove actual extra presents; counts can fluctuate because presentation is asynchronous. The game's FPS counter counts rendered frames only. For display pacing, NVIDIA recommends FrameView's `MsBetweenDisplayChange`; extra presents alone do not prove smooth timing.

**Game resolution smaller than the window.** When the game resolution differs from the window (for example 2560x1440 in a 2880x1800 borderless window), Unity renders at the game resolution and scales the image into the window, aspect-fit and centred with black bars. Frame Generation then runs on that rectangle: FSR's `generationRect` and DLSS-G's backbuffer subrect, onto which the render-sized depth and motion map. The HUD work stays at game resolution, where it is exact: the pre-HUD snapshot, the HUD opacity, and Unity's game-sized final image, read from Unity's screen buffer (bound when the HUD camera finishes, read at Present after overlays and IMGUI). The bridge places the final image, pre-HUD image, UI layer and opacity into the rectangle with bilinear filtering like Unity's blit and presents that copy, so they match. The screen buffer's row order is probed once per buffer against the pre-HUD image, repeating on later frames while a symmetric frame (a fade) cannot decide it. If Unity uses `SetSourceSize` instead, the top-left source region is used 1:1. Without a usable screen buffer, generation still uses the rectangle but has no HUD-less input or UI layer. Log: `FRAME_GEN_DISPLAY_MAPPING`, `FRAME_GEN_GAME_BUFFER`, `FRAME_BRIDGE_SOURCE_SIZE`. Fixture: `PipelineTests --fg [--fsr] [--fsr-upscaler] --display-scale` (16:9 game in a 16:10 chain, stale producer).

**Pacing.** Caps mean rendered (base) FPS: 30 is 30 game frames with generated frames added. The limiter keeps the game's FPS setting, calls Reflex sleep at frame start and then a native timer only if the interval has not elapsed, because a standalone driver test accepted Reflex caps without enforcing them. It never waits while holding the frame-token lock and does not burst to catch up. With VSync on, Unity ignores `targetFrameRate`, so a frame-start VSync guard enforces the cap (DLSS On or Off) while keeping the VSync setting. Turning upscaling off (F10, or quality Off) restores the game's limiter with the latest cap; F11 alone keeps Reflex pacing. Another mod writing a different Unity cap makes the limiter yield until the FPS setting changes again.

The overlay's limiter label shows `Reflex + guard 60 base FPS`, `Reflex unlimited`, `Unity + VSync guard 40 base FPS`, `Unity limiter (disabled)` or, with FSR, `Frame-start guard 60 base FPS`.

## Automated coverage

- `FrameBridgeTests --warp/--hardware`: 421 submissions with byte-exact readbacks, both Present APIs, tearing/waitable flags, D3D11 view guards, resize/recreation and retirement; `--fg-fallback` latches Frame Generation off on an unsupported adapter while presentation stays exact.
- `PipelineTests --hardware --bridge`: upscaling with the bridge, Streamline presentation bookkeeping.
- `build/Test.ps1 -FrameGeneration` (focused desktop): every multiplier 2–6 with actual SDK frame counts, live changes up/down and Off/on on one runtime, UI recomposition, application wait-handle mode. `-ReflexLimiter` adds hardware cap tests in both wait-handle modes.
- `FramePacingTests` measures 30/40/60 FPS pacing without graphics; managed tests cover the limiter's ownership, cap changes, unlimited, F10/F11 behavior, VSync, focus, pause, foreign-mod ownership, failures and disposal.

## Game test

Windowed or borderless, VSync off unless testing the guard.

1. Start with Frame Generation enabled. Enter gameplay at DLAA; F6 shows **D3D12 bridge**, **FG DLSS 2x enabled** and a rising **SDK presents** count. native.log: `FRAME_BRIDGE_READY`, `FRAME_GEN_ENABLED`, `FRAME_GEN_PRESENT ... sdkFrames=2`.
2. Close F6. Move, rotate, sprint; inspect moving enemies, particles, thin edges and HUD text. Compare F11 Off/On for smoothness, ghosting, HUD distortion and input response.
3. Cycle 2x → 4x → 6x → 3x → 2x in Mod Options without restarting: no restart prompt, `FRAME_GEN_MULTIPLIER` and the overlay show the effective multiplier.
4. Set the game FPS cap to 30, 60, then unlimited, at least 10 s each. The overlay shows `Reflex + guard 30 base FPS` etc., and the game's menu keeps your value. Compare F11 Off/On at each cap.
5. F10 Off: `Unity limiter (disabled)`, the game's own limiter applies the latest cap; change the cap from either menu, press F10 again and Reflex pacing resumes with it. An explicit unlimited stays unlimited.
6. Enable VSync at 40 FPS: `Unity + VSync guard 40 base FPS`, with DLSS On and Off and while paused. Disable VSync and alt-tab; focus loss returns pacing to Unity.
7. Test pause, alt-tab/minimize, resolution changes, a stage change, title/loading screens and relaunch. Generation suspends and resumes on fresh gameplay frames.
8. Restart with `Enabled = false`: native D3D11, no bridge. Enabling from Mod Options prompts for a restart; reverting to Off cancels it.
9. To compare against the game's original limiter, set `ReflexFrameLimiter = false` and relaunch.

For issues, send `managed.log`, `native.log`, `FrameGeneration/sl.log` and `LogOutput.log`, with the cap and the transition that triggered it. `FRAME_GEN_FAILED`, `FRAME_GEN_STATUS_FAILED`, `FRAME_GEN_ASYNC_PRESENT_ERROR` and `FRAME_BRIDGE_PRESENT_FAILED` need review.

## Known limits

SDR, windowed/borderless only. The limiter measures frame-start pacing, not display timing or input latency; it is not yet a pure driver-enforced Reflex limiter. Removing all loaded-SDK overhead while F11 is Off requires a restart with `Enabled = false`.
