#pragma once
#include "Upscaling/UpscalerContext.h"
#include "FrameGeneration/FrameGenerator.h"

__declspec(dllexport) void ConfigureFrameGeneration(bool, int multiplier=2, FrameGenerationBackend=FrameGenerationBackend::Auto);
bool FrameGenerationStart(IDXGIAdapter*); // Before D3D12CreateDevice.
bool FrameGenerationBind(ID3D12Device*);
ID3D12Device* FrameGenerationDevice();
bool FrameGenerationLoaded();
bool FrameGenerationWanted();
bool FrameGenerationUIAvailable();
__declspec(dllexport) void FrameGenerationFail(const char*);
// The presenting queue and display swap chain through the loaded runtime; throws.
void FrameGenerationCreateDisplay(ID3D12Device*, IDXGIFactory2*, HWND, const DXGI_SWAP_CHAIN_DESC1&,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, ComPtr<ID3D12CommandQueue>&, ComPtr<IDXGISwapChain1>&);
// After the bridge drained its GPU work, before it releases the display swap chain.
void FrameGenerationReleaseDisplay();
// Depth/motion use the final two arguments (render size); HUD-less color uses the display size.
bool FrameGenerationPrepare(const CameraFrame&, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, const DXGI_SWAP_CHAIN_DESC1&, UINT, UINT, UINT);
void FrameGenerationOff(const char*);
void FrameGenerationPresentMarker(bool, int);
bool FrameGenerationAfterPresent(FrameGenerationPresented&);
void FrameGenerationClearTags(int, unsigned width=0, unsigned height=0);
// The runtime composites the HUD from a separate UI layer (FSR).
bool FrameGenerationLayersUi();
// Before every display Present. True when the layer is registered: present the HUD-less image.
// Null, or a failure, unregisters it: present the final image.
bool FrameGenerationPresentUi(ID3D12Resource*);
extern "C" __declspec(dllexport) void __cdecl RiskOfRainUpscaling_FrameGenerationSetEnabled(int);
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_FrameGenerationState();
extern "C" __declspec(dllexport) void* __cdecl RiskOfRainUpscaling_FrameGenerationGetBeforeHudEvent();
extern "C" __declspec(dllexport) void* __cdecl RiskOfRainUpscaling_FrameGenerationGetAfterHudEvent();
extern "C" __declspec(dllexport) void __cdecl RiskOfRainUpscaling_FrameGenerationMarker(int, int);
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_FrameGenerationPresentedFrames();
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_FrameGenerationMultiplier();
// The running backend: 0 none, 1 DLSS Frame Generation, 2 FSR Frame Generation.
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_FrameGenerationKind();
// Queues a 2–6 multiplier for the presenting thread; never enables the runtime.
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_FrameGenerationSetMultiplier(int);
// Main frame-start caller; -1 means unavailable, otherwise the accepted cap in us.
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_ReflexSetFrameLimit(int);
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_ReflexFrameLimit();
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_ReflexGuardWaits();
