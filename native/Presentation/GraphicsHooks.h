#pragma once
#include "Core/Native.h"

// Early D3D11/DXGI hooks installed by the preloader: device and swap-chain
// creation, Present and buffer calls, routed through Streamline or the bridge.
int InstallGraphicsHooks();
bool VerifyGraphicsHooks();
int ObservedDeviceCount();
// Shared by production hooks and synthetic presentation tests.
__declspec(dllexport) void ObserveChain(IDXGISwapChain*);
