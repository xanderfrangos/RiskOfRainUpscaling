#include "Upscaling/UpscalerContext.h"
#include <cmath>
#include <cstdio>
#include <vector>

struct Pixel { float r,g,b,a; };
static ComPtr<ID3D11Texture2D> Texture(ID3D11Device* device, const std::vector<Pixel>* pixels=nullptr,
    DXGI_FORMAT format=DXGI_FORMAT_R32G32B32A32_FLOAT)
{
    D3D11_TEXTURE2D_DESC description{};description.Width=description.Height=4;description.MipLevels=description.ArraySize=1;
    description.Format=format;description.SampleDesc.Count=1;description.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{pixels?pixels->data():nullptr,4*sizeof(Pixel),0};
    ComPtr<ID3D11Texture2D> texture;
    if(FAILED(device->CreateTexture2D(&description,pixels?&initial:nullptr,&texture))) return nullptr;
    return texture;
}
static std::vector<Pixel> Read(ID3D11Device* device,ID3D11DeviceContext* immediate,ID3D11Texture2D* texture)
{
    D3D11_TEXTURE2D_DESC description{};texture->GetDesc(&description);description.BindFlags=0;description.Usage=D3D11_USAGE_STAGING;description.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if(FAILED(device->CreateTexture2D(&description,nullptr,&staging))) return {};
    immediate->CopyResource(staging.Get(),texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if(FAILED(immediate->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))) return {};
    std::vector<Pixel> result(16);
    for(int y=0;y<4;++y) for(int x=0;x<4;++x) result[y*4+x]=reinterpret_cast<Pixel*>(static_cast<unsigned char*>(mapped.pData)+y*mapped.RowPitch)[x];
    immediate->Unmap(staging.Get(),0);return result;
}
static bool Equal(float first,float second){return std::abs(first-second)<0.0002f;}
static ComPtr<ID3D11DepthStencilView> SceneDepth(ID3D11Device* device, const std::vector<Pixel>& pixels)
{
    std::vector<float> values;for(auto pixel:pixels)values.push_back(pixel.r);
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=4;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_R32_TYPELESS;
    desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    D3D11_SUBRESOURCE_DATA initial{values.data(),4*sizeof(float),0};ComPtr<ID3D11Texture2D> texture;
    if(FAILED(device->CreateTexture2D(&desc,&initial,&texture)))return nullptr;
    D3D11_DEPTH_STENCIL_VIEW_DESC view{};view.Format=DXGI_FORMAT_D32_FLOAT;view.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11DepthStencilView> result;if(FAILED(device->CreateDepthStencilView(texture.Get(),&view,&result)))return nullptr;return result;
}
int main()
{
    wchar_t current[32768]{};GetCurrentDirectoryW(32768,current);
    std::wstring root=current;
    RiskOfRainUpscaling_SetLoggingLevel(4); // Detailed fixture evidence.
    RiskOfRainUpscaling_Initialize((root+L"\\build\\test-results").c_str(),(root+L"\\build\\package").c_str(),L"scene-inputs-test");
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> immediate;
    if(FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&immediate))) return 1;
    std::vector<Pixel> depths(16),motions(16);
    for(int y=0;y<4;++y) for(int x=0;x<4;++x)
    {
        int i=y*4+x;depths[i]={0.1f+0.1f*x+0.1f*y,0,0,1};motions[i]={0.025f*x-0.05f,0.01f*y-0.02f,0,1};
    }
    static_assert(sizeof(SceneInputsDescription)==24,"Managed/native ABI mismatch");
    auto callback=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_SceneInputsGetEvent());
    auto frameEvent=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_SceneInputsGetFrameEvent());
    auto sceneDepth=SceneDepth(device.Get(),depths);if(!sceneDepth)return 2;
    for(auto format : {DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R32G32B32A32_TYPELESS})
    {
        auto depth=Texture(device.Get(),nullptr,format),motion=Texture(device.Get(),&motions),original=Texture(device.Get());
        if(!depth||!motion||!original) return 3;
        ComPtr<ID3D11RenderTargetView> target;
        if(FAILED(device->CreateRenderTargetView(original.Get(),nullptr,&target))) return 4;
        SceneInputsDescription description{sizeof(SceneInputsDescription),4,4,4,depth.Get()};
        auto invalid=description;invalid.size=1;if(RiskOfRainUpscaling_SceneInputsCreate(&invalid)) return 5;
        invalid=description;invalid.version=3;if(RiskOfRainUpscaling_SceneInputsCreate(&invalid)) return 6;
        invalid=description;invalid.width=8;if(RiskOfRainUpscaling_SceneInputsCreate(&invalid)) return 7;
        invalid=description;invalid.depth=nullptr;if(RiskOfRainUpscaling_SceneInputsCreate(&invalid)) return 8;
        if(format==DXGI_FORMAT_R32G32B32A32_TYPELESS)
        {
            ComPtr<ID3D11ShaderResourceView> inferred;
            // Reproduce the original game failure: an untyped view cannot be inferred.
            if(device->CreateShaderResourceView(depth.Get(),nullptr,&inferred)!=E_INVALIDARG) return 9;
        }
        auto context=RiskOfRainUpscaling_SceneInputsCreate(&description);if(!context) return 10;
        // Inputs captured without a fresh geometry probe are rejected.
        auto pointer=target.Get();immediate->OMSetRenderTargets(1,&pointer,sceneDepth.Get());
        callback(100,context);if(RiskOfRainUpscaling_SceneInputsState(context)!=-2) return 11;
        callback(199,context);RiskOfRainUpscaling_SceneInputsDestroy(context);
        context=RiskOfRainUpscaling_SceneInputsCreate(&description);if(!context) return 12;
        D3D11_VIEWPORT viewport{1,2,3,4,0,1};immediate->RSSetViewports(1,&viewport);
        frameEvent(7,context);callback(90,context);callback(100,context);
        // Native depth copies must leave Unity's bindings and viewport intact.
        ComPtr<ID3D11RenderTargetView> unchanged;immediate->OMGetRenderTargets(1,&unchanged,nullptr);
        D3D11_VIEWPORT restored{};UINT count=1;immediate->RSGetViewports(&count,&restored);
        if(unchanged.Get()!=target.Get()||count!=1||restored.TopLeftX!=1||restored.TopLeftY!=2||restored.Width!=3||RiskOfRainUpscaling_SceneInputsState(context)!=1)return 13;
        if(SceneInputsSceneColor(context,7,device.Get())!=original.Get()||SceneInputsSceneColor(context,8,device.Get()))return 14;
        if(SceneInputsReady(context,7,device.Get()))return 15; // Motion not validated yet.
        ComPtr<ID3D11ShaderResourceView> motionSource;
        if(FAILED(device->CreateShaderResourceView(motion.Get(),nullptr,&motionSource))) return 16;
        auto source=motionSource.Get();immediate->PSSetShaderResources(0,1,&source);callback(102,context);
        SceneInputsMetrics metrics{};
        if(SceneInputsGetMetrics(context,&metrics)||metrics.frame!=1||metrics.depthWidth!=4||metrics.motionWidth!=4||metrics.gbufferWidth!=4||
            metrics.geometryDepthWidth!=4||metrics.stageMask!=14) return 17;
        if(!SceneInputsReady(context,7,device.Get())||SceneInputsReady(context,8,device.Get()))return 18;
        auto values=Read(device.Get(),immediate.Get(),depth.Get());if(values.size()!=16)return 19;
        for(int i=0;i<16;++i)if(!Equal(values[i].r,depths[i].r)||!Equal(values[i].g,depths[i].r)||!Equal(values[i].a,1))return 20;
        callback(199,context);if(RiskOfRainUpscaling_SceneInputsState(context)!=-1) return 21;RiskOfRainUpscaling_SceneInputsDestroy(context);
    }
    // Real depth allocations can have packed stencil and no source SRV bind.
    // The private copy must provide the typed SRV without changing Unity's DSV.
    for(auto format:{DXGI_FORMAT_D24_UNORM_S8_UINT,DXGI_FORMAT_D32_FLOAT_S8X24_UINT,DXGI_FORMAT_D16_UNORM})
    {
        auto raw=Texture(device.Get()),color=Texture(device.Get());
        SceneInputsDescription description{sizeof(SceneInputsDescription),4,4,4,raw.Get()};
        auto context=RiskOfRainUpscaling_SceneInputsCreate(&description);if(!context)return 22;
        D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=4;desc.MipLevels=desc.ArraySize=1;desc.Format=format;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11DepthStencilView> dsv;
        if(FAILED(device->CreateTexture2D(&desc,nullptr,&texture))||FAILED(device->CreateDepthStencilView(texture.Get(),nullptr,&dsv)))return 23;
        immediate->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,.25f,0);
        ComPtr<ID3D11RenderTargetView> target;
        if(FAILED(device->CreateRenderTargetView(color.Get(),nullptr,&target)))return 24;
        auto pointer=target.Get();immediate->OMSetRenderTargets(1,&pointer,dsv.Get());callback(90,context);
        if(RiskOfRainUpscaling_SceneInputsState(context)!=1)return 25;
        // Emulate the release of the DSV before transparency finishes: depth is
        // captured once, from the buffer bound at this frame's G-buffer probe.
        immediate->OMSetRenderTargets(1,&pointer,nullptr);callback(100,context);
        auto values=Read(device.Get(),immediate.Get(),raw.Get());
        if(RiskOfRainUpscaling_SceneInputsState(context)!=1||values.size()!=16)return 27;
        for(auto pixel:values)if(!Equal(pixel.r,.25f)||!Equal(pixel.g,.25f)||!Equal(pixel.b,.25f))return 26;
        // The G-buffer depth serves only its own frame: a later frame without a
        // bound DSV and without a probe is rejected.
        callback(100,context);if(RiskOfRainUpscaling_SceneInputsState(context)!=-2)return 28;
        callback(199,context);RiskOfRainUpscaling_SceneInputsDestroy(context);
    }
    // Unity RFloat raw depth (typeless or typed): one channel, written once after transparents.
    for(auto format:{DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_R32_FLOAT})
    {
        D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=4;desc.MipLevels=desc.ArraySize=1;desc.Format=format;desc.SampleDesc.Count=1;
        desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> raw;if(FAILED(device->CreateTexture2D(&desc,nullptr,&raw)))return 29;
        auto color=Texture(device.Get());ComPtr<ID3D11RenderTargetView> target;
        if(!color||FAILED(device->CreateRenderTargetView(color.Get(),nullptr,&target)))return 30;
        SceneInputsDescription description{sizeof(SceneInputsDescription),4,4,4,raw.Get()};
        auto context=RiskOfRainUpscaling_SceneInputsCreate(&description);if(!context)return 31;
        auto pointer=target.Get();immediate->OMSetRenderTargets(1,&pointer,sceneDepth.Get());
        frameEvent(9,context);callback(90,context);callback(100,context);
        if(RiskOfRainUpscaling_SceneInputsState(context)!=1)return 32;
        desc.Format=DXGI_FORMAT_R32_TYPELESS;desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;if(FAILED(device->CreateTexture2D(&desc,nullptr,&staging)))return 33;
        immediate->CopyResource(staging.Get(),raw.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};if(FAILED(immediate->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)))return 34;
        bool exact=true;
        for(int y=0;y<4;++y)for(int x=0;x<4;++x)exact=exact&&Equal(reinterpret_cast<float*>(static_cast<unsigned char*>(mapped.pData)+y*mapped.RowPitch)[x],depths[y*4+x].r);
        immediate->Unmap(staging.Get(),0);if(!exact)return 35;
        callback(199,context);RiskOfRainUpscaling_SceneInputsDestroy(context);
    }
    printf("PASS Scene inputs: native DSV depth copy, G-buffer probe, single capture with same-frame fallback, typed/typeless RGBA32F and R32F depth targets, stencil formats, motion validation, readiness, scene color, state restoration, retirement, ABI.\n");
    return 0;
}
