#pragma once
#include "Core/Native.h"
#include <memory>

// Replays HUD geometry into an opacity-only target. Original game draws and
// their RGB/alpha, depth and stencil state are left intact.
class HudOpacity {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    __declspec(dllexport) HudOpacity(ID3D11Device*, ID3D11DeviceContext*, UINT, UINT);
    __declspec(dllexport) ~HudOpacity();
    __declspec(dllexport) void Begin(ID3D11Texture2D*, int);
    __declspec(dllexport) bool End(int);
    __declspec(dllexport) void Cancel();
    __declspec(dllexport) ID3D11Texture2D* Texture() const;
    __declspec(dllexport) const char* Reason() const;
};
