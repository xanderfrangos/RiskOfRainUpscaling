#include "FrameGeneration/FrameBridge.h"
#include "Presentation/GraphicsHooks.h"
#include "FrameGeneration/FrameGeneration.h"
#include "FrameGeneration/FrameGenerationMode.h"
#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc,char** argv){
    for(int multiplier=2;multiplier<=6;++multiplier){
        if(SupportedFrameGenerationMultiplier(multiplier,5)!=multiplier ||
           SupportedFrameGenerationMultiplier(multiplier,1)!=2 ||
           SupportedFrameGenerationMultiplier(multiplier,3)!=std::min(multiplier,4) ||
           SupportedFrameGenerationMultiplier(multiplier,0)!=0)return 45;
    }
    if(SupportedFrameGenerationMultiplier(99,UINT32_MAX)!=6 || SupportedFrameGenerationMultiplier(-1,5)!=2)return 46;
    bool hardware=argc>1 && !std::strcmp(argv[1],"--hardware");
    bool fgFallback=argc>2 && !std::strcmp(argv[2],"--fg-fallback");
    if(fgFallback){if(hardware)return 43;ConfigureFrameGeneration(true);}
    wchar_t path[32768]{};GetCurrentDirectoryW(32768,path);std::wstring root=path;
    RiskOfRainUpscaling_SetLoggingLevel(4); // Detailed fixture evidence.
    RiskOfRainUpscaling_Initialize((root+L"\\build\\test-results").c_str(),(root+L"\\build\\package\\BepInEx\\plugins\\RiskOfRainUpscaling\\runtimes").c_str(),L"frame-bridge-fixture");
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    if(FAILED(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)))return 1;
    ComPtr<IDXGIFactory2> factory;if(FAILED(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory))))return 2;
    HWND window=CreateWindowExW(0,L"STATIC",L"Frame bridge fixture",WS_OVERLAPPEDWINDOW,0,0,96,96,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);if(!window)return 3;
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=64;desc.Height=32;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1;desc.BufferCount=2;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;desc.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT|DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    ComPtr<IDXGIFactory5> factory5;BOOL tearing=FALSE;
    if(SUCCEEDED(factory.As(&factory5)))factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&tearing,sizeof(tearing));
    if(tearing)desc.Flags|=DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    ComPtr<IDXGISwapChain1> chain;auto bad=desc;bad.SampleDesc.Count=2;
    if(CreateFrameBridge(factory.Get(),device.Get(),window,&bad,nullptr,nullptr,&chain)!=DXGI_ERROR_UNSUPPORTED || chain)return 4;
    if(FAILED(CreateFrameBridge(factory.Get(),device.Get(),window,&desc,nullptr,nullptr,&chain)))return 5;
    if(fgFallback && RiskOfRainUpscaling_FrameGenerationState()!=-2)return 44;
    if(RiskOfRainUpscaling_FrameGenerationSetMultiplier(4)!=-1)return 47; // Off/failed runtimes cannot be enabled by a multiplier.
    ObserveChain(chain.Get()); // Exercise real hooks plus the replacement object.
    ComPtr<IDXGISwapChain2> chain2;if(FAILED(chain.As(&chain2)))return 6;
    ComPtr<IUnknown> base, versionIdentity;
    if(FAILED(chain.As(&base)) || FAILED(chain2.As(&versionIdentity)) || base.Get()!=versionIdentity.Get())return 7;
    base.Reset();versionIdentity.Reset();
    ComPtr<IDXGISwapChain3> unsupported;if(chain.As(&unsupported)!=E_NOINTERFACE || unsupported)return 8;
    ComPtr<ID3D11Device> owner;if(FAILED(chain->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get()!=device.Get())return 9;
    ComPtr<ID3D12Device> wrong;if(chain->GetDevice(IID_PPV_ARGS(&wrong))!=E_NOINTERFACE)return 10;
    ComPtr<ID3D11Texture2D> producer;
    if(FAILED(chain->GetBuffer(0,IID_PPV_ARGS(&producer))))return 11;
    D3D11_TEXTURE2D_DESC td{};producer->GetDesc(&td);
    if(td.Width!=64 || td.Height!=32 || td.Format!=DXGI_FORMAT_R8G8B8A8_TYPELESS)return 12;
    ComPtr<ID3D11RenderTargetView> view;
    D3D11_RENDER_TARGET_VIEW_DESC vd{};vd.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;vd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
    if(FAILED(device->CreateRenderTargetView(producer.Get(),&vd,&view)))return 13;
    if(chain->ResizeBuffers(2,80,48,DXGI_FORMAT_UNKNOWN,desc.Flags)!=DXGI_ERROR_INVALID_CALL)return 14;
    if(!chain2->GetFrameLatencyWaitableObject() || FAILED(chain2->SetMaximumFrameLatency(2)))return 15;
    UINT latency=0;if(FAILED(chain2->GetMaximumFrameLatency(&latency)) || latency!=2)return 16;
    auto target=view.Get();context->OMSetRenderTargets(1,&target,nullptr);
    if(FAILED(chain->Present(0,DXGI_PRESENT_TEST)))return 17;
    ComPtr<ID3D11RenderTargetView> bound;context->OMGetRenderTargets(1,&bound,nullptr);if(bound.Get()!=view.Get())return 18;bound.Reset();
    std::vector<unsigned char> pixels(64*32*4),readback(pixels.size());
    for(unsigned frame=0;frame<120;++frame){
        for(size_t i=0;i<pixels.size();++i)pixels[i]=static_cast<unsigned char>((i*7+frame*13+(i/256)*11)&255);
        context->UpdateSubresource(producer.Get(),0,nullptr,pixels.data(),64*4,0);
        auto oldProducer=producer.Get();
        DXGI_PRESENT_PARAMETERS params{};
        UINT flags=tearing?DXGI_PRESENT_ALLOW_TEARING:0;
        HRESULT hr=frame%2?chain->Present1(0,flags,&params):chain->Present(0,flags);
        if(FAILED(hr))return 19;
        if(FAILED(FrameBridgeReadback(chain.Get(),readback.data(),static_cast<unsigned>(readback.size()))) || pixels!=readback)return 20;
        ComPtr<ID3D11Texture2D> reacquired;if(FAILED(chain->GetBuffer(0,IID_PPV_ARGS(&reacquired))) || reacquired.Get()!=oldProducer)return 21;
    }
    context->OMGetRenderTargets(1,&bound,nullptr);if(bound)return 22;
    // Exercise a pipelined burst without readbacks serializing every frame.
    for(unsigned frame=120;frame<420;++frame){
        for(size_t i=0;i<pixels.size();++i)pixels[i]=static_cast<unsigned char>((i*23+frame*17)&255);
        context->UpdateSubresource(producer.Get(),0,nullptr,pixels.data(),64*4,0);
        if(FAILED(chain->Present(0,tearing?DXGI_PRESENT_ALLOW_TEARING:0)))return 33;
    }
    if(FAILED(FrameBridgeReadback(chain.Get(),readback.data(),static_cast<unsigned>(readback.size()))) || pixels!=readback)return 34;
    if(chain->Present(5,0)!=DXGI_ERROR_INVALID_CALL)return 23;
    if(chain->SetFullscreenState(TRUE,nullptr)!=DXGI_ERROR_UNSUPPORTED)return 24;
    producer.Reset(); // Keep the indirect RTV reference: it must still prevent resize.
    if(chain->ResizeBuffers(2,80,48,DXGI_FORMAT_UNKNOWN,desc.Flags)!=DXGI_ERROR_INVALID_CALL)return 25;
    view.Reset();context->ClearState();context->Flush();
    // SRVs and the newer D3D11.3 view API must obey the same lifetime guard.
    if(FAILED(chain->GetBuffer(0,IID_PPV_ARGS(&producer))))return 35;
    ComPtr<ID3D11ShaderResourceView> sampled;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
    if(FAILED(device->CreateShaderResourceView(producer.Get(),&sd,&sampled)))return 36;
    producer.Reset();
    if(chain->ResizeBuffers(2,80,48,DXGI_FORMAT_UNKNOWN,desc.Flags)!=DXGI_ERROR_INVALID_CALL)return 37;
    sampled.Reset();
    ComPtr<ID3D11Device3> device3;if(FAILED(device.As(&device3)))return 38;
    if(FAILED(chain->GetBuffer(0,IID_PPV_ARGS(&producer))))return 39;
    D3D11_RENDER_TARGET_VIEW_DESC1 vd1{};vd1.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;vd1.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView1> view1;if(FAILED(device3->CreateRenderTargetView1(producer.Get(),&vd1,&view1)))return 40;
    auto boundOnly=static_cast<ID3D11RenderTargetView*>(view1.Get());context->OMSetRenderTargets(1,&boundOnly,nullptr);
    producer.Reset();view1.Reset();
    if(chain->ResizeBuffers(2,80,48,DXGI_FORMAT_UNKNOWN,desc.Flags)!=DXGI_ERROR_INVALID_CALL)return 41;
    context->ClearState();context->Flush();
    if(FAILED(chain->ResizeBuffers(3,80,48,DXGI_FORMAT_UNKNOWN,desc.Flags)))return 26;
    DXGI_SWAP_CHAIN_DESC1 resized{};if(FAILED(chain->GetDesc1(&resized)) || resized.Width!=80 || resized.Height!=48 || resized.BufferCount!=3 || resized.Flags!=desc.Flags)return 27;
    if(FAILED(chain->GetBuffer(0,IID_PPV_ARGS(&producer))))return 28;
    if(FrameBridgeReadback(chain.Get(),readback.data(),80*48*4)!=E_INVALIDARG)return 42;
    pixels.resize(80*48*4);readback.resize(pixels.size());for(size_t i=0;i<pixels.size();++i)pixels[i]=static_cast<unsigned char>(i*19);
    context->UpdateSubresource(producer.Get(),0,nullptr,pixels.data(),80*4,0);
    if(FAILED(chain->Present(0,0)) || FAILED(FrameBridgeReadback(chain.Get(),readback.data(),static_cast<unsigned>(readback.size()))) || pixels!=readback)return 29;
    producer.Reset();chain2.Reset();auto raw=chain.Detach();if(raw->Release()!=0 || RiskOfRainUpscaling_GetPresentationState()==2)return 30;
    context->Flush();
    if(FAILED(CreateFrameBridge(factory.Get(),device.Get(),window,&desc,nullptr,nullptr,&chain)))return 31;
    ObserveChain(chain.Get());raw=chain.Detach();if(raw->Release()!=0)return 32;
    DestroyWindow(window);
    printf("PASS: %s D3D11-to-D3D12 bridge; 421 submissions, 122 byte-exact readbacks (all RGBA channels), pipelined burst, both Present APIs, TEST, tearing/waitable flags, native D3D11 ownership, COM identity, direct/RTV/SRV/bound D3D11.3 view guards, resize/recreation and retirement. FG OFF.\n",hardware?"hardware":"WARP");
    if(fgFallback)printf("PASS: unsupported FG adapter latches interpolation off while ordinary D3D12 presentation remains byte-exact.\n");
    return 0;
}
