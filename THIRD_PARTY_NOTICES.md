# Third-party notices

Risk of Rain Upscaling's own code is under the [MIT License](LICENSE). It uses and redistributes the following third-party components, which remain under their own licenses. Built packages include each license file next to the mod's DLLs.

| Component | Version | Used for | License | Source |
| --- | --- | --- | --- | --- |
| NVIDIA Streamline SDK (`sl.*.dll`, headers) | 2.14.1 | DLSS, DLSS Frame Generation and Reflex integration | MIT (`Streamline-LICENSE.txt`) | [NVIDIA-RTX/Streamline](https://github.com/NVIDIA-RTX/Streamline) |
| NVIDIA DLSS runtimes (`nvngx_dlss.dll`, `nvngx_dlssg.dll`) | 310.9.1 | DLSS Super Resolution and Frame Generation | NVIDIA DLSS SDK license | [NVIDIA/DLSS](https://github.com/NVIDIA/DLSS) |
| AMD FidelityFX SDK (signed DX12 DLLs, API headers) | 2.3.0 | FSR upscaling and FSR Frame Generation | AMD FidelityFX SDK license (`FidelityFX-SDK-LICENSE.md`) | [GPUOpen-LibrariesAndSDKs/FidelityFX-SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) |
| AMD FidelityFX FSR 1 (`ffx_a.h`, `ffx_fsr1.h`) | commit `a21ffb8` | RCAS sharpening | MIT (`FidelityFX-FSR-LICENSE.txt`) | [GPUOpen-Effects/FidelityFX-FSR](https://github.com/GPUOpen-Effects/FidelityFX-FSR) |

The NVIDIA runtimes are committed in [third_party/NVIDIA-DLSS](third_party/NVIDIA-DLSS). The Streamline headers and the FidelityFX SDK are downloaded by `build/Fetch-SDK.ps1` at pinned commits. FSR 1 is vendored in [third_party/FidelityFX-FSR](third_party/FidelityFX-FSR).

## Trademarks

NVIDIA, RTX, DLSS and Reflex are trademarks of NVIDIA Corporation. AMD, FidelityFX and FSR are trademarks of Advanced Micro Devices, Inc. Risk of Rain 2 is published by Gearbox Publishing. This project is not affiliated with or endorsed by NVIDIA, AMD, Hopoo Games or Gearbox.
