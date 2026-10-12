## v1.0.0
- Initial release.

## v1.0.1
- Updated package README and included repository in manifest.

## v1.0.2
- Fixed FSR Frame Generation not activating properly.
- Fixed handling of the game's resolution being different from the display/window's resolution.
- Renamed "DLAA" to "DLAA/Native AA".
- Added upscaler and frame generation support to the intro sequence.
- Fixed various minor performance/rendering bugs.

## v1.1.0
- Added "FSR 3 DX11" upscaler: AMD FSR 3.1 running directly on the game's DirectX 11 renderer, without the DirectX 12 bridge. Auto now uses it on GPUs without DLSS unless FSR 4 is available. The existing setting is now called "FSR3/4 DX12".
- Added DLSS Dynamic Multi-Frame Generation (DMFG) support. Like with other frame generation modes, the FPS/Reflex cap will be used to limit the base FPS. Use driver-level settings for adjusting the desired output FPS.