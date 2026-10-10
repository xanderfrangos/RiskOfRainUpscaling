#pragma once
#include "Core/D3D11StateScope.h"
#include <stdexcept>

// Shared fullscreen-triangle pass for native contexts that draw on Unity's
// immediate context: owned pipeline state, typed views of Unity textures and a
// device-state bracket that keeps Unity's cached bindings intact.
struct FullscreenPass
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediate;
    ComPtr<ID3D11DeviceContext1> immediate1;
    ComPtr<ID3DDeviceContextState> visualState;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11DepthStencilState> depthState;
    ComPtr<ID3D11BlendState> blend;
    void Create(ID3D11Device* owner, const void* vertexShader, size_t vertexSize);
    void Draw(ID3D11PixelShader* shader, ID3D11ShaderResourceView* first, ID3D11ShaderResourceView* second,
        ID3D11RenderTargetView* output, int width, int height, ID3D11Buffer* constants = nullptr);
    D3D11StateScope Scope() { return {immediate1.Get(), visualState.Get()}; }
    void Reset();
};

// Logs "<tag> <operation> HRESULT" and throws on failure.
void Require(HRESULT result, const char* operation, const char* tag);
// Typed format for our owned linear Unity ARGBFloat, ARGBHalf, ARGB32, RFloat and RGHalf textures.
DXGI_FORMAT LinearViewFormat(DXGI_FORMAT format);
// Typed SRV (input) or RTV view of an owned single-sample Unity texture of the given size.
void OwnedView(ID3D11Device* device, void* pointer, const char* label, bool input, int width, int height, void** output);
