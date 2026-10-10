#include "Core/FullscreenPass.h"

void Require(HRESULT result, const char* operation, const char* tag)
{
    if (FAILED(result)) { Log(ModLogLevel::Error, "%s %s HRESULT=0x%08lx", tag, operation, result); throw std::runtime_error(operation); }
}

DXGI_FORMAT LinearViewFormat(DXGI_FORMAT format)
{
    // A typeless allocation needs an explicit typed view; nullptr cannot infer it.
    switch (format)
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    // Unity RFloat raw depth and RGHalf raw motion.
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM: return format;
    default: throw std::runtime_error("Owned texture has unsupported format");
    }
}

void OwnedView(ID3D11Device* device, void* pointer, const char* label, bool input, int width, int height, void** output)
{
    if (!pointer) throw std::runtime_error("Owned texture is null");
    auto resource = static_cast<ID3D11Resource*>(pointer);
    ComPtr<ID3D11Device> owner; resource->GetDevice(&owner);
    if (owner.Get() != device) throw std::runtime_error("Owned textures belong to different devices");
    ComPtr<ID3D11Texture2D> texture;
    Require(resource->QueryInterface(IID_PPV_ARGS(&texture)), "Owned texture interface", "OWNED_TEXTURE_FAILED");
    D3D11_TEXTURE2D_DESC description{}; texture->GetDesc(&description);
    Log(ModLogLevel::Debug, "OWNED_TEXTURE %s size=%ux%u format=%u bind=0x%x samples=%u array=%u mips=%u input=%d", label,
        description.Width, description.Height, description.Format, description.BindFlags, description.SampleDesc.Count,
        description.ArraySize, description.MipLevels, input);
    if (description.Width != static_cast<UINT>(width) || description.Height != static_cast<UINT>(height) ||
        description.SampleDesc.Count != 1 || description.ArraySize != 1 || description.MipLevels < 1 ||
        !(description.BindFlags & (input ? D3D11_BIND_SHADER_RESOURCE : D3D11_BIND_RENDER_TARGET)))
        throw std::runtime_error("Owned texture dimensions/bind flags unsupported");
    auto format = LinearViewFormat(description.Format);
    HRESULT result;
    if (input)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC view{}; view.Format = format; view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        result = device->CreateShaderResourceView(texture.Get(), &view, reinterpret_cast<ID3D11ShaderResourceView**>(output));
    }
    else
    {
        D3D11_RENDER_TARGET_VIEW_DESC view{}; view.Format = format; view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        result = device->CreateRenderTargetView(texture.Get(), &view, reinterpret_cast<ID3D11RenderTargetView**>(output));
    }
    Log(ModLogLevel::Debug, "OWNED_VIEW %s type=%s typedFormat=%u HRESULT=0x%08lx", label, input ? "SRV" : "RTV", format, result);
    Require(result, input ? "Owned texture SRV" : "Owned texture RTV", "OWNED_TEXTURE_FAILED");
}

void FullscreenPass::Create(ID3D11Device* owner, const void* vertexShader, size_t vertexSize)
{
    device = owner;
    device->GetImmediateContext(&immediate);
    ComPtr<ID3D11Device1> device1; Require(device.As(&device1), "ID3D11Device1", "FULLSCREEN_PASS_FAILED");
    Require(immediate.As(&immediate1), "ID3D11DeviceContext1", "FULLSCREEN_PASS_FAILED");
    auto level = device->GetFeatureLevel();
    Require(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &visualState), "Visual state", "FULLSCREEN_PASS_FAILED");
    Require(device->CreateVertexShader(vertexShader, vertexSize, nullptr, &vertex), "Vertex shader", "FULLSCREEN_PASS_FAILED");
    D3D11_SAMPLER_DESC sample{}; sample.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sample.AddressU = sample.AddressV = sample.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sample.MaxLOD = D3D11_FLOAT32_MAX;
    Require(device->CreateSamplerState(&sample, &sampler), "Sampler", "FULLSCREEN_PASS_FAILED");
    D3D11_RASTERIZER_DESC raster{}; raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE; raster.DepthClipEnable = TRUE;
    Require(device->CreateRasterizerState(&raster, &rasterizer), "Rasterizer", "FULLSCREEN_PASS_FAILED");
    D3D11_DEPTH_STENCIL_DESC depth{};
    Require(device->CreateDepthStencilState(&depth, &depthState), "Depth state", "FULLSCREEN_PASS_FAILED");
    D3D11_BLEND_DESC opaque{}; opaque.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    Require(device->CreateBlendState(&opaque, &blend), "Blend state", "FULLSCREEN_PASS_FAILED");
}

void FullscreenPass::Draw(ID3D11PixelShader* shader, ID3D11ShaderResourceView* first, ID3D11ShaderResourceView* second,
    ID3D11RenderTargetView* output, int width, int height, ID3D11Buffer* constants)
{
    auto context = immediate.Get();
    ID3D11ShaderResourceView* unbound[2]{}; context->PSSetShaderResources(0, 2, unbound);
    context->OMSetRenderTargets(1, &output, nullptr);
    float factors[4]{}; context->OMSetBlendState(blend.Get(), factors, 0xffffffff);
    context->OMSetDepthStencilState(depthState.Get(), 0); context->RSSetState(rasterizer.Get());
    D3D11_VIEWPORT viewport{0,0,static_cast<float>(width),static_cast<float>(height),0,1}; context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(nullptr); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex.Get(), nullptr, 0); context->PSSetShader(shader, nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0); context->HSSetShader(nullptr, nullptr, 0); context->DSSetShader(nullptr, nullptr, 0);
    ID3D11ShaderResourceView* sources[]{first,second}; context->PSSetShaderResources(0, 2, sources);
    auto point = sampler.Get(); context->PSSetSamplers(0, 1, &point);
    context->PSSetConstantBuffers(0, 1, &constants);
    context->Draw(3,0);
    context->PSSetShaderResources(0, 2, unbound);
}

void FullscreenPass::Reset()
{
    blend.Reset(); depthState.Reset(); rasterizer.Reset(); sampler.Reset(); vertex.Reset();
    visualState.Reset(); immediate1.Reset(); immediate.Reset(); device.Reset();
}
