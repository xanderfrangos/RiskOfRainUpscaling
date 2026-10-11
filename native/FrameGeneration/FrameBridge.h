#pragma once
#include "Core/CameraFrame.h"
#include "Core/Native.h"

// Unity remains D3D11; optional, separately owned D3D12 Frame Generation.
void ConfigureFrameBridge();
bool BuildingFrameBridge();
bool FrameBridgeRequested();
// [Frame Generation] Enabled at startup: as configured by the preloader, or read from
// the config file when the preloader never initialized (early hooks disabled or missed).
bool FrameGenerationRequestedAtStartup();
bool FrameBridgeWindowEligible(HWND);
// Called on the D3D11 render thread after a successful DLAA evaluation.
// outputWidth/outputHeight: Unity's game resolution (the upscaler output); 0 means the chain's size.
void FrameBridgeSubmitInputs(const CameraFrame&, ID3D11Texture2D*, ID3D11Texture2D*, UINT outputWidth = 0, UINT outputHeight = 0);
void FrameBridgeBeforeHud(int, bool captureUi=true);
void FrameBridgeAfterHud(int, bool);
void FrameBridgeGameBuffer(int);
void FrameBridgeRenderFrame(int);
__declspec(dllexport) HRESULT CreateFrameBridge(IDXGIFactory2*, ID3D11Device*, HWND,
    const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_GetPresentationState();
// The presenting queue on queueDevice and the window's D3D12 swap chain; throws.
void CreateBridgeDisplay(ID3D12Device* queueDevice, IDXGIFactory2*, HWND, const DXGI_SWAP_CHAIN_DESC1&,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, ComPtr<ID3D12CommandQueue>&, ComPtr<IDXGISwapChain1>&);
// Standalone test access: copies the latest displayed buffer after GPU completion.
__declspec(dllexport) HRESULT FrameBridgeReadback(IDXGISwapChain*, void*, unsigned);
// Typed D3D12 FG snapshots: 1 depth (float), 2 motion (half2), 3 pre-UI color (RGBA8), 4 UI alpha (R8),
// 5 FSR UI layer (premultiplied RGBA8).
__declspec(dllexport) HRESULT FrameBridgeInputReadback(IDXGISwapChain*, int, void*, unsigned);
