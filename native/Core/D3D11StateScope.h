#pragma once
#include "Core/Native.h"

// A native draw owns its entire state bracket. Never let Unity commands run
// between these swaps: Unity's cached bindings would no longer be accurate.
struct D3D11StateScope
{
    ID3D11DeviceContext1* context;
    ComPtr<ID3DDeviceContextState> saved;
    D3D11StateScope(ID3D11DeviceContext1* value, ID3DDeviceContextState* state) : context(value) { context->SwapDeviceContextState(state, &saved); }
    ~D3D11StateScope() { context->SwapDeviceContextState(saved.Get(), nullptr); }
    D3D11StateScope(const D3D11StateScope&) = delete;
    D3D11StateScope& operator=(const D3D11StateScope&) = delete;
};
