#pragma once
#include "Core/Native.h"
#include <functional>
#include <mutex>
#include <sl.h>

// Serialize evaluation, interface upgrades, and presentation for the owned SDK.
extern std::recursive_mutex streamlineMutex;
void EnableStreamlinePresentation(ID3D11Device*, PFun_slUpgradeInterface*);
bool StreamlinePresentationHealthy();
void FailStreamlinePresentation();
void TrackStreamlineSwapChain(IDXGISwapChain*);
// True while the early hooks track a game swap chain. DLSS presents only through
// those chains, so without one (hooks missed or disabled) it cannot run.
bool StreamlinePresentationObserved();
bool RouteThroughStreamline(IDXGISwapChain*, const std::function<void(IDXGISwapChain*)>&);
ULONG ReleaseStreamlineSwapChain(IUnknown* identity, ULONG remaining);
void RecordStreamlinePresent(UINT flags, bool nativeCalled, HRESULT result);
__declspec(dllexport) unsigned StreamlinePresentedFrames();
