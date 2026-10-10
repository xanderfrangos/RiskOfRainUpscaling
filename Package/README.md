# Risk of Rain Upscaling

Introduces upscaling and frame generation to Risk of Rain 2, using **NVIDIA DLSS** or **AMD FSR**.

- **Upscaling:** the game renders at a lower resolution and the mod rebuilds a clean full-resolution image. The game is already light, so depending on your system and FPS, you may lose frames instead of gaining any. Consider this an anti-aliasing option more than performance saving at high framerates. 
- **Frame Generation:** adds generated frames between the real ones for smoother motion: up to 6x on NVIDIA, 2x with FSR.

[Risk of Options](https://thunderstore.io/package/Rune580/Risk_Of_Options/) is optional but strongly recommended. It adds an in-game settings page.

## Will it work on my PC?

| Graphics card | Upscaling | Frame Generation |
|-|-|-|
| NVIDIA RTX 50 series | DLSS | DLSS, 2x–6x |
| NVIDIA RTX 40 series | DLSS | DLSS, 2x |
| NVIDIA RTX 20 and 30 series | DLSS | FSR, 2x |
| AMD, Intel and others | FSR | FSR, 2x |

The mod picks the right option for your card automatically. Only Windows + NVIDIA has been tested as of writing.

## Getting started

1. **Launch the game.** The mod starts in DLAA / Native AA: better anti-aliasing at your normal resolution.
2. **Want more FPS?** Go to **Settings → Mod Options → Risk of Rain Upscaling** and set **Upscaler Quality** to **Quality** or **Balanced**. Lower settings are faster but softer.
3. **Want smoother motion?** Turn on **Frame Generation**, then restart the game.

## FAQ

**Frame Generation won't turn on.** Restart the game after enabling it, and don't use exclusive fullscreen.

**The image looks too sharp or too soft.** Adjust **Sharpening** (0 turns it off, 1 is strongest).

**How do I turn it off?** Set the upscaling level to "Off".

**I found a bug.** Set **Logging Level** to **Info**, reproduce the problem, then open an issue on the mod's GitHub page and attach the `BepInEx/RiskOfRainUpscaling-Diagnostics` folder from your profile.

## Manual installation

1. Install BepInEx 5.
2. Extract the `BepInEx` folder from this package into the game folder, keeping its `plugins` and `patchers` subfolders.

## Options

Change settings in **Settings → Mod Options → Risk of Rain Upscaling**, or edit `BepInEx/config/xanderfrangos.riskofrainupscaling.cfg`.

|Section|Settings|
|-|-|
|`[General]`|`Enabled`: master switch for everything the mod does.|
|`[Upscaling]`|`Enabled`, `Quality` (default DLAA), `Upscaler` (Auto, DLSS, FSR), `DLSS Preset`, `Outlines`, `Sharpening`, `MipBiasOffset`, `MainMenu`.|
|`[Frame Generation]`|`Enabled` (default off), `Multiplier`, `Backend` (Auto, DLSS, FSR), `ReflexFrameLimiter`. Turning Frame Generation on when it was off at startup needs a restart; changing `Backend` always does.|
|`[Diagnostics]`|`Logging Level` (default Off), `ShowOverlay`, `CaptureWhileOff`, `TraceGraphicsStartup` (leave on; Frame Generation needs it).|
|`[Hotkeys]`|`Enable Hotkeys` (default off) and five rebindable shortcuts.|

## Hotkeys

You probably don't need these. Turn on **Hotkeys → Enable Hotkeys** to use these shortcuts.

|Default|Action|
|-|-|
|F10|Toggle upscaling|
|F7|Cycle upscaler quality|
|F11|Toggle Frame Generation|
|F6|Debug overlay|
|F8|Capture diagnostics|

## License

MIT. Bundles NVIDIA DLSS/Streamline and AMD FidelityFX under their own licenses (included in the plugin folder). Not affiliated with NVIDIA, AMD, Hopoo Games or Gearbox.
