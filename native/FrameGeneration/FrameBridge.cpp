#include "FrameGeneration/FrameBridge.h"
#include "FrameGeneration/FrameGeneration.h"
#include "FrameGeneration/HudOpacity.h"
#include "Diagnostics/Profiler.h"
#include "shaders/Fullscreen.h"
#include "shaders/UiLayer.h"
#include "shaders/UiMask.h"
#include "shaders/UiMaskAlpha.h"
#include "shaders/FlipRows.h"
#include "shaders/Letterbox.h"
#include <array>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <cstring>
#include <filesystem>
#include <map>
#include <fstream>
#include <vector>

static std::atomic<int> requested{0}, active{0}, creationFailures{0}, runtimeFailures{0};
static thread_local bool building=false;
static std::atomic<HWND> selectedWindow{nullptr};
struct BuildingScope { bool previous=building; BuildingScope(){building=true;} ~BuildingScope(){building=previous;} };
bool BuildingFrameBridge(){return building;}
bool FrameBridgeRequested(){return requested.load()!=0;}
bool FrameBridgeWindowEligible(HWND window){
    if(!FrameBridgeRequested() || BuildingFrameBridge() || !IsWindow(window))return false;
    DWORD process{};GetWindowThreadProcessId(window,&process);
    if(process!=GetCurrentProcessId() || (GetWindowLongPtrW(window,GWL_STYLE)&WS_CHILD))return false;
    HWND selected=selectedWindow.load();
    return !selected || selected==window || !IsWindow(selected);
}
extern "C" int __cdecl RiskOfRainUpscaling_GetPresentationState(){
    if(runtimeFailures.load())return -2;
    if(active.load())return 2;
    if(creationFailures.load())return -1;
    return requested.load(); // 0 native; 1 bridge requested but no active chain; 2 active; -1 creation failure.
}
static bool bridgeConfigured=false;
static std::wstring ConfigPath(){
    return (std::filesystem::path(g_logFolder).parent_path()/L"config"/L"xanderfrangos.riskofrainupscaling.cfg").wstring();
}
// The preloader binds/saves the same preferences used by Mod Options before
// any swap-chain creation, so a missing entry (standalone tests) stays Off.
static bool SavedFrameGenerationEnabled(const std::wstring& config,wchar_t (&preference)[16]){
    GetPrivateProfileStringW(L"Frame Generation",L"Enabled",L"",preference,16,config.c_str());
    return !_wcsicmp(preference,L"true");
}
bool FrameGenerationRequestedAtStartup(){
    if(bridgeConfigured)return FrameBridgeRequested();
    wchar_t preference[16]{};return SavedFrameGenerationEnabled(ConfigPath(),preference);
}
void ConfigureFrameBridge(){
    if(bridgeConfigured)return;
    bridgeConfigured=true;
    const auto config=ConfigPath();
    wchar_t preference[16]{};const bool enabled=SavedFrameGenerationEnabled(config,preference);
    wchar_t value[32]{};GetPrivateProfileStringW(L"Frame Generation",L"Multiplier",L"2",value,32,config.c_str());
    wchar_t* end{};long parsed=wcstol(value,&end,10);
    int multiplier=end!=value && !*end?static_cast<int>(std::clamp(parsed,2L,6L)):2;
    // Auto: DLSS Frame Generation on supported NVIDIA adapters, otherwise FSR.
    wchar_t backendName[16]{};GetPrivateProfileStringW(L"Frame Generation",L"Backend",L"Auto",backendName,16,config.c_str());
    auto backend=!_wcsicmp(backendName,L"DLSS")?FrameGenerationBackend::DLSS:!_wcsicmp(backendName,L"FSR")?FrameGenerationBackend::FSR:FrameGenerationBackend::Auto;
    requested=enabled?1:0;
    ConfigureFrameGeneration(enabled,multiplier,backend);
    Log("FRAME_BRIDGE_STARTUP mode=%ls config=%ls FrameGeneration=%ls preference=%ls multiplier=%dx backend=%ls; presentation runtime fixed for this session; Off/on and multipliers can change live in D3D12; restart to enable from D3D11 or change backend",enabled?L"Bridge":L"Native",config.c_str(),enabled?L"On":L"Off",preference,multiplier,backend==FrameGenerationBackend::DLSS?L"DLSS":backend==FrameGenerationBackend::FSR?L"FSR":L"Auto");
}
// 32-bit top-down BMP from BGRA rows; diagnostics only.
static bool WriteBmp(const std::filesystem::path& path,UINT width,UINT height,const std::vector<unsigned char>& bgra){
    BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=static_cast<LONG>(width);info.biHeight=-static_cast<LONG>(height);
    info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;info.biSizeImage=static_cast<DWORD>(bgra.size());
    BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info);file.bfSize=file.bfOffBits+info.biSizeImage;
    std::ofstream out(path,std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file),sizeof(file));out.write(reinterpret_cast<const char*>(&info),sizeof(info));
    out.write(reinterpret_cast<const char*>(bgra.data()),static_cast<std::streamsize>(bgra.size()));
    return static_cast<bool>(out);
}
static void RequireBridge(HRESULT hr,const char* name){
    if(FAILED(hr)){Log(ModLogLevel::Error, "FRAME_BRIDGE_FAILED %s HRESULT=0x%08lx",name,hr);throw std::runtime_error(name);}
}
// D3D11 views keep resources alive with internal references, which texture
// AddRef/Release cannot count. A private COM token follows actual view lifetime,
// including bindings. Only views of our tagged producer receive the token.
static const GUID producerViewToken={0x6d0627f6,0x0f31,0x4bd9,{0x84,0x9a,0x2d,0x83,0x01,0x43,0x8b,0xb0}};
class ViewToken final : public IUnknown {
    std::atomic<ULONG> refs{1};
public:
    ULONG Count()const{return refs.load();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{
        if(!out)return E_POINTER;*out=nullptr;if(iid!=__uuidof(IUnknown))return E_NOINTERFACE;*out=this;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release()override{auto value=--refs;if(!value)delete this;return value;}
};
static std::mutex viewHookMutex;
static std::map<void**,void*> viewOriginals;
template<class Device,class Desc,class View,int Index>
static HRESULT STDMETHODCALLTYPE CreateTrackedView(Device* device,ID3D11Resource* resource,const Desc* desc,View** result){
    using Fn=HRESULT(STDMETHODCALLTYPE*)(Device*,ID3D11Resource*,const Desc*,View**);
    Fn original{};
    {std::lock_guard<std::mutex> guard(viewHookMutex);original=reinterpret_cast<Fn>(viewOriginals.at(*reinterpret_cast<void***>(device)+Index));}
    auto hr=original(device,resource,desc,result);
    if(SUCCEEDED(hr) && resource && result && *result){
        ComPtr<IUnknown> token;UINT size=sizeof(IUnknown*);
        if(SUCCEEDED(resource->GetPrivateData(producerViewToken,&size,token.GetAddressOf())) && token){
            auto tagged=(*result)->SetPrivateDataInterface(producerViewToken,token.Get());
            if(FAILED(tagged)){(*result)->Release();*result=nullptr;return tagged;}
        }
    }
    return hr;
}
static void TrackProducerViews(ID3D11Device* device){
    std::lock_guard<std::mutex> guard(viewHookMutex);
    auto patch=[](void* object,int index,void* hook){
        auto slot=*reinterpret_cast<void***>(object)+index;
        if(*slot==hook)return;
        DWORD protection{};RequireBridge(VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&protection)?S_OK:HRESULT_FROM_WIN32(GetLastError()),"Producer-view hook protection");
        viewOriginals[slot]=*slot;InterlockedExchangePointer(slot,hook);
        DWORD ignored{};VirtualProtect(slot,sizeof(void*),protection,&ignored);
    };
    patch(device,7,reinterpret_cast<void*>(&CreateTrackedView<ID3D11Device,D3D11_SHADER_RESOURCE_VIEW_DESC,ID3D11ShaderResourceView,7>));
    patch(device,9,reinterpret_cast<void*>(&CreateTrackedView<ID3D11Device,D3D11_RENDER_TARGET_VIEW_DESC,ID3D11RenderTargetView,9>));
    ComPtr<ID3D11Device3> device3;
    if(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device3)))){
        patch(device3.Get(),57,reinterpret_cast<void*>(&CreateTrackedView<ID3D11Device3,D3D11_SHADER_RESOURCE_VIEW_DESC1,ID3D11ShaderResourceView1,57>));
        patch(device3.Get(),59,reinterpret_cast<void*>(&CreateTrackedView<ID3D11Device3,D3D11_RENDER_TARGET_VIEW_DESC1,ID3D11RenderTargetView1,59>));
    }
}
struct BridgeHandle {
    HANDLE value{};
    ~BridgeHandle(){if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
// An RGBA8 image in typeless storage: UNORM view and target, plus an sRGB target to
// encode float sources the way Unity's blit to the linear-space swap chain does.
struct GameImage {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11RenderTargetView> target,srgbTarget;
    UINT width=0,height=0;
    void Create(ID3D11Device* device,UINT w,UINT h,const char* name){
        Reset();
        D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;d.SampleDesc.Count=1;
        d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        RequireBridge(device->CreateTexture2D(&d,nullptr,&texture),name);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
        RequireBridge(device->CreateShaderResourceView(texture.Get(),&sd,&view),name);
        D3D11_RENDER_TARGET_VIEW_DESC rd{};rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;rd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
        RequireBridge(device->CreateRenderTargetView(texture.Get(),&rd,&target),name);
        rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        RequireBridge(device->CreateRenderTargetView(texture.Get(),&rd,&srgbTarget),name);
        width=w;height=h;
    }
    void Reset(){texture.Reset();view.Reset();target.Reset();srgbTarget.Reset();width=height=0;}
};
struct BridgeSlot {
    ComPtr<ID3D11Texture2D> shared11;
    ComPtr<ID3D12Resource> shared12, backbuffer;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    UINT64 done=0;
    ComPtr<ID3D11Texture2D> depth11,motion11,hudless11;
    ComPtr<ID3D12Resource> depth12,motion12,hudless12;
    ComPtr<ID3D12Resource> depthTag12,motionTag12,hudlessTag12;
    ComPtr<ID3D11Texture2D> ui11;
    ComPtr<ID3D12Resource> ui12,uiTag12;
    // FSR: the HUD as premultiplied RGBA8, registered with the swap chain (which copies it at Present).
    ComPtr<ID3D11Texture2D> layer11;
    ComPtr<ID3D12Resource> layer12;
    ComPtr<ID3D11ShaderResourceView> finalView;
    ComPtr<ID3D11RenderTargetView> layerView;
    ComPtr<ID3D11RenderTargetView> sharedTarget,hudlessTarget,uiTarget; // Placement of game-sized images when Unity scales.
    ComPtr<ID3D11Query> coverage; // Occlusion query: opaque difference-mask pixels.
    bool coveragePending=false;
    ComPtr<ID3D12Fence> fgFence;
    UINT64 fgDone=0;
    UINT inputWidth=0,inputHeight=0; // Depth/motion render size; DLSS can be below display size.
    bool uiCopied=false;
    // Profiling: start/end of the copy list and a timestamp after Present.
    ComPtr<ID3D12QueryHeap> timeHeap;
    ComPtr<ID3D12Resource> timeReadback;
    ComPtr<ID3D12CommandAllocator> timeAllocator;
    ComPtr<ID3D12GraphicsCommandList> timeList;
    bool timed=false;
};
struct BridgeGpu {
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context11;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> display;
    ComPtr<ID3D12Fence> produced12, consumed12;
    ComPtr<ID3D11Fence> produced11;
    BridgeHandle event;
    ComPtr<ID3D11Texture2D> producer;
    // Game-sized images: the swap chain's size unless Unity scales a smaller game
    // resolution into it (GameRect). hudless is the exact pre-HUD snapshot; finalGame
    // Unity's final image at game size when it scales; layerGame the UI layer there.
    GameImage hudless,finalGame,layerGame;
    // Bottom-up or float sources are copied here (RGBA8 bytes, or RGBA16F), then drawn top-down.
    ComPtr<ID3D11Texture2D> rows,floatRows;
    ComPtr<ID3D11ShaderResourceView> rowsView,floatRowsView;
    // Unity's game-sized screen buffer this frame (only while it scales), and its probed row order.
    ComPtr<ID3D11Texture2D> gameBuffer;
    bool gameBufferFloat=false,probedFloat=false,gameBufferBottomUp=false,gameBufferUsable=false;
    const void* probedBuffer=nullptr; // Never dereferenced.
    const void* inconclusiveBuffer=nullptr; // Logging only.
    std::unique_ptr<HudOpacity> opacity;
    bool opacityUnavailable=false;
    ComPtr<ID3DDeviceContextState> layerState;
    ComPtr<ID3D11VertexShader> layerVertex;
    ComPtr<ID3D11PixelShader> layerPixel,maskPixel,maskAlphaPixel,flipPixel,letterboxPixel;
    ComPtr<ID3D11Buffer> placementConstants;
    ComPtr<ID3D11SamplerState> linearClamp;
    ComPtr<ID3D11Query> probeQuery;
    ComPtr<ID3D11ShaderResourceView> opacityView;
    ID3D11Texture2D* opacityViewSource=nullptr; // The view holds a reference, so the address stays unique.
    bool layerUnavailable=false;
    // DLSS-G ran with UI recomposition last frame. Turning it off and on reconfigures
    // the runtime (a ~150 ms stall), so a frame whose HUD capture failed keeps it on.
    bool uiRecomposing=false;
    int layerSource=-1,layerLogs=0; // Logging only.
    // An opaque difference mask covering most of the screen pastes the rendered
    // frame over every generated one. While it does, FSR's HUD-less detection
    // is used instead; the mask is still measured so the layer can return.
    bool layerCovering=false;
    int coverageHigh=0,coverageLow=0,coverageSamples=0,coverageReports=0;
    double coverageSum=0,coverageMax=0,coverageArea=1;
    bool coverageDumped=false;
    ComPtr<ViewToken> views;
    ComPtr<ID3D12Resource> pendingReadback;
    std::array<BridgeSlot,3> slots;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    UINT64 serial=0;
    UINT lastIndex=0;
    bool submitted=false, hasImage=false;
    bool fg=false;
    UINT applicationFlags=0;
    bool unknownFgCompletion=false;
    void WaitFg(BridgeSlot& slot){
        if(!slot.fgFence || !slot.fgDone)return;
        auto deadline=GetTickCount64()+5000;
        RequireBridge(slot.fgFence->SetEventOnCompletion(slot.fgDone,event.value),"FG input-consumption event");
        for(;;){
            auto completed=slot.fgFence->GetCompletedValue();
            if(completed==UINT64_MAX)throw std::runtime_error("FG device removed");
            if(completed>=slot.fgDone){slot.fgFence.Reset();slot.fgDone=0;return;}
            auto now=GetTickCount64();
            if(now>=deadline || WaitForSingleObject(event.value,static_cast<DWORD>(deadline-now))!=WAIT_OBJECT_0)throw std::runtime_error("FG input-consumption timeout");
        }
    }
    void Wait(UINT64 value,bool nonblocking=false){
        if(!value)return;
        UINT64 completed=consumed12->GetCompletedValue();
        if(completed==UINT64_MAX)throw std::runtime_error("D3D12 device removed");
        if(completed>=value)return;
        if(nonblocking)throw std::runtime_error("Frame slot busy");
        RequireBridge(consumed12->SetEventOnCompletion(value,event.value),"Completion event");
        auto deadline=GetTickCount64()+5000;
        for(;;){
            completed=consumed12->GetCompletedValue();
            if(completed==UINT64_MAX)throw std::runtime_error("D3D12 device removed");
            if(completed>=value)return;
            auto now=GetTickCount64();
            if(now>=deadline || WaitForSingleObject(event.value,static_cast<DWORD>(deadline-now))!=WAIT_OBJECT_0)throw std::runtime_error("GPU completion timeout");
            // A previous timed-out registration can signal this same event.
            // Verify the requested fence value before permitting resource reuse.
        }
    }
    bool Drain(){
        if(unknownFgCompletion)return false;
        if(!submitted)return true;
        try{
            auto value=++serial;RequireBridge(queue->Signal(consumed12.Get(),value),"Drain signal");Wait(value);
            for(auto& slot:slots)WaitFg(slot);
            return true;
        }catch(...){return false;}
    }
    void Buffers(){
        RequireBridge(display->GetDesc1(&desc),"Display description");
        if(desc.BufferCount<2 || desc.BufferCount>slots.size())throw std::runtime_error("Unsupported display buffer count");
        D3D11_TEXTURE2D_DESC p{};p.Width=desc.Width;p.Height=desc.Height;p.MipLevels=p.ArraySize=1;
        // Unity can create both linear and sRGB views of its render target.
        p.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;p.SampleDesc.Count=1;
        p.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        RequireBridge(device11->CreateTexture2D(&p,nullptr,&producer),"Unity-facing producer");
        views.Attach(new ViewToken());
        RequireBridge(producer->SetPrivateDataInterface(producerViewToken,views.Get()),"Producer-view lifetime token");
        hasImage=false;
        for(UINT i=0;i<desc.BufferCount;++i){
            auto& slot=slots[i];p.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
            p.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            RequireBridge(device11->CreateTexture2D(&p,nullptr,&slot.shared11),"Shared color slot");
            ComPtr<IDXGIResource1> resource;RequireBridge(slot.shared11.As(&resource),"Shared resource interface");
            BridgeHandle handle;RequireBridge(resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle.value),"Shared color handle");
            RequireBridge(device12->OpenSharedHandle(handle.value,IID_PPV_ARGS(&slot.shared12)),"Open shared color on D3D12");
            RequireBridge(display->GetBuffer(i,IID_PPV_ARGS(&slot.backbuffer)),"D3D12 display buffer");
            RequireBridge(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&slot.allocator)),"Slot allocator");
            RequireBridge(device12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,slot.allocator.Get(),nullptr,IID_PPV_ARGS(&slot.list)),"Slot list");
            RequireBridge(slot.list->Close(),"Initial slot close");
        }
    }
    void SharedInput(DXGI_FORMAT format,ComPtr<ID3D11Texture2D>& texture11,ComPtr<ID3D12Resource>& texture12,UINT width,UINT height){
        D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=1;d.Format=format;d.SampleDesc.Count=1;
        d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;d.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        RequireBridge(device11->CreateTexture2D(&d,nullptr,&texture11),"FG shared input");
        ComPtr<IDXGIResource1> resource;RequireBridge(texture11.As(&resource),"FG shared resource");
        BridgeHandle handle;RequireBridge(resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle.value),"FG input shared handle");
        RequireBridge(device12->OpenSharedHandle(handle.value,IID_PPV_ARGS(&texture12)),"FG D3D12 input");
    }
    // Callers wait for this slot's FG input fence before resizing its inputs.
    void Inputs(BridgeSlot& slot,UINT width,UINT height,bool layered){
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        auto owned=[&](ID3D12Resource* shared,ComPtr<ID3D12Resource>& tagged){
            auto d=shared->GetDesc();d.Flags=D3D12_RESOURCE_FLAG_NONE;
            RequireBridge(device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&tagged)),"FG presenting-queue snapshot");
        };
        if(!slot.hudless11){
            SharedInput(DXGI_FORMAT_R8G8B8A8_UNORM,slot.hudless11,slot.hudless12,desc.Width,desc.Height);
            owned(slot.hudless12.Get(),slot.hudlessTag12);
        }
        if(layered && !slot.layer11)SharedInput(DXGI_FORMAT_R8G8B8A8_UNORM,slot.layer11,slot.layer12,desc.Width,desc.Height);
        if(!layered && opacity && !slot.ui11 && !opacityUnavailable){
            try {SharedInput(DXGI_FORMAT_R8_UNORM,slot.ui11,slot.ui12,desc.Width,desc.Height);owned(slot.ui12.Get(),slot.uiTag12);}
            catch(const std::exception& e){slot.ui11.Reset();slot.ui12.Reset();slot.uiTag12.Reset();opacityUnavailable=true;Log(ModLogLevel::Warning, "FRAME_GEN_UI_FALLBACK %s; ordinary HUDless FG retained",e.what());}
        }
        if(slot.depth11 && slot.inputWidth==width && slot.inputHeight==height)return;
        slot.depth11.Reset();slot.depth12.Reset();slot.depthTag12.Reset();
        slot.motion11.Reset();slot.motion12.Reset();slot.motionTag12.Reset();slot.inputWidth=slot.inputHeight=0;
        SharedInput(DXGI_FORMAT_R32_FLOAT,slot.depth11,slot.depth12,width,height);
        SharedInput(DXGI_FORMAT_R16G16_FLOAT,slot.motion11,slot.motion12,width,height);
        owned(slot.depth12.Get(),slot.depthTag12);owned(slot.motion12.Get(),slot.motionTag12);
        slot.inputWidth=width;slot.inputHeight=height;
    }
    // Reads a finished coverage query: the share of display pixels the opaque mask marks as HUD.
    void ReadCoverage(BridgeSlot& slot){
        UINT64 pixels=0;
        if(!slot.coveragePending || context11->GetData(slot.coverage.Get(),&pixels,sizeof(pixels),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK)return;
        slot.coveragePending=false;
        const double share=static_cast<double>(pixels)/coverageArea;
        coverageSum+=share;coverageMax=std::max(coverageMax,share);++coverageSamples;
        coverageHigh=share>=.5?coverageHigh+1:0;coverageLow=share<.25?coverageLow+1:0;
        if(!layerCovering && coverageHigh>=8){
            layerCovering=true;
            Log(ModLogLevel::Warning,"FRAME_GEN_UI_LAYER_COVERING coverage=%.1f%%; the opaque HUD difference mask hides generated frames; FSR HUD-less detection until it falls below 25%%",100*share);
        }else if(layerCovering && coverageLow>=30){
            layerCovering=false;
            Log("FRAME_GEN_UI_LAYER_RESTORED coverage=%.1f%%; UI layer composited by the swap chain again",100*share);
        }
        if((!coverageReports && coverageSamples==1) || coverageSamples>=150){
            if(coverageReports<64){
                ++coverageReports;
                Log("FRAME_GEN_UI_LAYER_COVERAGE samples=%d mean=%.2f%% max=%.2f%% last=%.2f%% (share of display pixels the opaque difference mask marks as HUD) fallback=%s",
                    coverageSamples,100*coverageSum/coverageSamples,100*coverageMax,100*share,layerCovering?"FSR HUD-less detection":"off");
            }
            coverageSamples=0;coverageSum=coverageMax=0;
        }
    }
    // At Debug logging, once per session when the mask first covers the screen: the
    // final image, the HUD-less snapshot and their per-pixel differences, to identify
    // what changes pixels outside the HUD. Synchronous readback and ~44 MB of files
    // on the render thread, so never at the default logging level.
    void DumpCoverage(ID3D11Texture2D* finalSource){
        coverageDumped=true;
        D3D11_TEXTURE2D_DESC d{};hudless.texture->GetDesc(&d);
        d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> finalCopy,hudlessCopy;
        D3D11_MAPPED_SUBRESOURCE a{},b{};
        if(FAILED(device11->CreateTexture2D(&d,nullptr,&finalCopy)) || FAILED(device11->CreateTexture2D(&d,nullptr,&hudlessCopy))){Log(ModLogLevel::Warning,"FRAME_GEN_UI_LAYER_DUMP_FAILED staging textures");return;}
        context11->CopyResource(finalCopy.Get(),finalSource);context11->CopyResource(hudlessCopy.Get(),hudless.texture.Get());
        if(FAILED(context11->Map(finalCopy.Get(),0,D3D11_MAP_READ,0,&a))){Log(ModLogLevel::Warning,"FRAME_GEN_UI_LAYER_DUMP_FAILED map");return;}
        if(FAILED(context11->Map(hudlessCopy.Get(),0,D3D11_MAP_READ,0,&b))){context11->Unmap(finalCopy.Get(),0);Log(ModLogLevel::Warning,"FRAME_GEN_UI_LAYER_DUMP_FAILED map");return;}
        const UINT width=d.Width,height=d.Height;
        std::vector<unsigned char> finalImage(size_t(width)*height*4),hudlessImage(finalImage.size()),diffImage(finalImage.size());
        // Largest RGB channel difference: 0, 1, 2, 3-4, 5-8, 9-16, 17-64, 65+.
        const int limits[]{0,1,2,4,8,16,64,255};UINT64 histogram[8]{};
        double signedSum[3]{};UINT64 alphaOnly=0;
        for(UINT y=0;y<height;++y){
            auto f=static_cast<const unsigned char*>(a.pData)+size_t(y)*a.RowPitch;
            auto h=static_cast<const unsigned char*>(b.pData)+size_t(y)*b.RowPitch;
            for(UINT x=0;x<width;++x){
                const size_t i=(size_t(y)*width+x)*4;int largest=0;
                for(int c=0;c<3;++c){const int delta=int(f[x*4+c])-int(h[x*4+c]);signedSum[c]+=delta;largest=std::max(largest,std::abs(delta));}
                if(!largest && f[x*4+3]!=h[x*4+3])++alphaOnly;
                int bucket=0;while(largest>limits[bucket])++bucket;++histogram[bucket];
                for(int c=0;c<3;++c){finalImage[i+c]=f[x*4+2-c];hudlessImage[i+c]=h[x*4+2-c];}
                finalImage[i+3]=hudlessImage[i+3]=255;
                const auto shown=static_cast<unsigned char>(std::min(255,largest*32));
                diffImage[i]=diffImage[i+1]=diffImage[i+2]=shown;diffImage[i+3]=255;
            }
        }
        auto f=static_cast<const unsigned char*>(a.pData)+size_t(height/2)*a.RowPitch+size_t(width/2)*4;
        auto h=static_cast<const unsigned char*>(b.pData)+size_t(height/2)*b.RowPitch+size_t(width/2)*4;
        Log("FRAME_GEN_UI_LAYER_DIFF size=%ux%u largestRGBDelta: 0=%llu 1=%llu 2=%llu 3-4=%llu 5-8=%llu 9-16=%llu 17-64=%llu 65+=%llu; meanSigned(final-hudless) r=%.3f g=%.3f b=%.3f; alphaOnly=%llu; centre final=%u,%u,%u,%u hudless=%u,%u,%u,%u",
            width,height,histogram[0],histogram[1],histogram[2],histogram[3],histogram[4],histogram[5],histogram[6],histogram[7],
            signedSum[0]/(double(width)*height),signedSum[1]/(double(width)*height),signedSum[2]/(double(width)*height),alphaOnly,
            f[0],f[1],f[2],f[3],h[0],h[1],h[2],h[3]);
        context11->Unmap(hudlessCopy.Get(),0);context11->Unmap(finalCopy.Get(),0);
        std::error_code error;const auto folder=std::filesystem::path(g_logFolder)/L"ui-layer-dump";std::filesystem::create_directories(folder,error);
        const bool written=WriteBmp(folder/L"final.bmp",width,height,finalImage) && WriteBmp(folder/L"hudless.bmp",width,height,hudlessImage) && WriteBmp(folder/L"difference-x32.bmp",width,height,diffImage);
        Log("FRAME_GEN_UI_LAYER_DUMP folder=%ls written=%d (final.bmp, hudless.bmp, difference-x32.bmp: largest RGB delta x32)",folder.c_str(),written);
    }
    // Private context state, shaders and states for the bridge's full-screen passes.
    void LayerShaders(){
        if(layerState)return;
        ComPtr<ID3D11Device1> device1;RequireBridge(device11.As(&device1),"D3D11.1 device");
        auto level=device11->GetFeatureLevel();
        RequireBridge(device1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),nullptr,&layerState),"UI layer state");
        RequireBridge(device11->CreateVertexShader(FullscreenShader,sizeof(FullscreenShader),nullptr,&layerVertex),"UI layer vertex shader");
        RequireBridge(device11->CreatePixelShader(UiLayerShader,sizeof(UiLayerShader),nullptr,&layerPixel),"UI layer shader");
        RequireBridge(device11->CreatePixelShader(UiMaskShader,sizeof(UiMaskShader),nullptr,&maskPixel),"UI mask shader");
        RequireBridge(device11->CreatePixelShader(UiMaskAlphaShader,sizeof(UiMaskAlphaShader),nullptr,&maskAlphaPixel),"UI mask alpha shader");
        RequireBridge(device11->CreatePixelShader(FlipRowsShader,sizeof(FlipRowsShader),nullptr,&flipPixel),"HUD-less row flip shader");
        RequireBridge(device11->CreatePixelShader(LetterboxShader,sizeof(LetterboxShader),nullptr,&letterboxPixel),"Game image placement shader");
        D3D11_BUFFER_DESC b{};b.ByteWidth=16;b.Usage=D3D11_USAGE_DEFAULT;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        RequireBridge(device11->CreateBuffer(&b,nullptr,&placementConstants),"Game image placement constants");
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
        RequireBridge(device11->CreateSamplerState(&sd,&linearClamp),"Game image placement sampler");
    }
    // One full-screen pass in the private state over width x height of target, with
    // inputs at t0.. and, for placement, the game rectangle and a linear sampler.
    void Draw(ID3D11PixelShader* pixel,ID3D11ShaderResourceView* const* inputs,UINT inputCount,ID3D11RenderTargetView* target,UINT width,UINT height,
        ID3D11Query* query=nullptr,const DisplayRect* placement=nullptr){
        LayerShaders();
        if(placement){
            const float rect[4]{static_cast<float>(placement->left),static_cast<float>(placement->top),static_cast<float>(placement->width),static_cast<float>(placement->height)};
            context11->UpdateSubresource(placementConstants.Get(),0,nullptr,rect,0,0);
        }
        ComPtr<ID3DDeviceContextState> previous;context11->SwapDeviceContextState(layerState.Get(),&previous);
        context11->OMSetRenderTargets(1,&target,nullptr);
        D3D11_VIEWPORT vp{0,0,static_cast<float>(width),static_cast<float>(height),0,1};context11->RSSetViewports(1,&vp);
        context11->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context11->VSSetShader(layerVertex.Get(),nullptr,0);context11->PSSetShader(pixel,nullptr,0);
        if(placement){auto c=placementConstants.Get();context11->PSSetConstantBuffers(0,1,&c);auto sampler=linearClamp.Get();context11->PSSetSamplers(0,1,&sampler);}
        context11->PSSetShaderResources(0,inputCount,inputs);
        if(query)context11->Begin(query);
        context11->Draw(3,0);
        if(query)context11->End(query);
        // Leave no bindings in the private state: these textures are copied into every frame.
        ID3D11ShaderResourceView* none[3]{};context11->PSSetShaderResources(0,3,none);
        ID3D11RenderTargetView* noTarget=nullptr;context11->OMSetRenderTargets(1,&noTarget,nullptr);
        context11->SwapDeviceContextState(previous.Get(),nullptr);
    }
    // A game-sized image placed where Unity scales the game into the swap chain.
    void Place(ID3D11ShaderResourceView* source,ID3D11RenderTargetView* target,const DisplayRect& rect){
        Draw(letterboxPixel.Get(),&source,1,target,desc.Width,desc.Height,nullptr,&rect);
    }
    ID3D11RenderTargetView* SlotTarget(ComPtr<ID3D11RenderTargetView>& view,ID3D11Texture2D* texture){
        if(!view)RequireBridge(device11->CreateRenderTargetView(texture,nullptr,&view),"Slot placement target");
        return view.Get();
    }
    ID3D11ShaderResourceView* OpacityView(ID3D11Texture2D* hudOpacity){
        if(!hudOpacity)return nullptr;
        if(hudOpacity!=opacityViewSource){
            opacityView.Reset();opacityViewSource=nullptr;
            RequireBridge(device11->CreateShaderResourceView(hudOpacity,nullptr,&opacityView),"UI layer opacity view");
            opacityViewSource=hudOpacity;
        }
        return opacityView.Get();
    }
    // Game-sized images follow Unity's game resolution, which equals the swap chain
    // unless Unity scales a smaller game image into it.
    void GameImages(UINT width,UINT height){
        if(hudless.width==width && hudless.height==height)return;
        hudless.Create(device11.Get(),width,height,"Exact before-HUD snapshot");
        finalGame.Create(device11.Get(),width,height,"Game-sized final image");
        layerGame.Create(device11.Get(),width,height,"Game-sized UI layer");
        rows.Reset();rowsView.Reset();floatRows.Reset();floatRowsView.Reset();
        opacity.reset();opacityView.Reset();opacityViewSource=nullptr;
        layerCovering=false;coverageHigh=coverageLow=coverageSamples=0;coverageSum=coverageMax=0;
        probedBuffer=nullptr;
    }
    // A game-sized source as destination's top-down 8-bit bytes. Unity renders into
    // RenderTextures bottom-up on D3D. Float (RGBA16F) targets are written to the
    // linear-space swap chain through an sRGB view, so the same hardware encoding is
    // used here; RGBA8 bytes are copied unchanged.
    void ConvertInto(GameImage& destination,ID3D11Texture2D* source,bool bottomUp,bool floatSource){
        if(!bottomUp && !floatSource){context11->CopyResource(destination.texture.Get(),source);return;}
        LayerShaders();
        D3D11_TEXTURE2D_DESC d{};destination.texture->GetDesc(&d);
        const UINT width=d.Width,height=d.Height;
        if(!floatSource && !rows){
            d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
            RequireBridge(device11->CreateTexture2D(&d,nullptr,&rows),"Row copy");
            RequireBridge(device11->CreateShaderResourceView(rows.Get(),nullptr,&rowsView),"Row copy view");
        }
        if(floatSource && !floatRows){
            d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            RequireBridge(device11->CreateTexture2D(&d,nullptr,&floatRows),"Float row copy");
            RequireBridge(device11->CreateShaderResourceView(floatRows.Get(),nullptr,&floatRowsView),"Float row copy view");
        }
        context11->CopyResource(floatSource?floatRows.Get():rows.Get(),source); // Same format family: raw copy.
        auto input=floatSource?floatRowsView.Get():rowsView.Get();
        auto target=floatSource?destination.srgbTarget.Get():destination.target.Get();
        if(bottomUp)Draw(flipPixel.Get(),&input,1,target,width,height);
        else {const DisplayRect whole{0,0,width,height};Draw(letterboxPixel.Get(),&input,1,target,width,height,nullptr,&whole);}
    }
    // Pixels where final differs from the HUD-less snapshot. Synchronous: only for
    // the one-time row-order probe of a new game buffer.
    UINT64 Differences(ID3D11ShaderResourceView* final){
        if(!probeQuery){D3D11_QUERY_DESC q{D3D11_QUERY_OCCLUSION,0};RequireBridge(device11->CreateQuery(&q,&probeQuery),"Game buffer probe query");}
        const float clear[4]{};context11->ClearRenderTargetView(layerGame.target.Get(),clear);
        ID3D11ShaderResourceView* inputs[2]{final,hudless.view.Get()};
        Draw(maskPixel.Get(),inputs,2,layerGame.target.Get(),hudless.width,hudless.height,probeQuery.Get());
        UINT64 pixels=0;const auto deadline=GetTickCount64()+500;
        while(context11->GetData(probeQuery.Get(),&pixels,sizeof(pixels),0)!=S_OK){if(GetTickCount64()>deadline)return UINT64_MAX;Sleep(0);}
        return pixels;
    }
    // Unity's game-sized final image when it scales the game into a larger chain:
    // with SetSourceSize the producer's top-left region, otherwise its game-sized
    // screen buffer, recorded when the HUD camera finished (overlay canvases and
    // IMGUI draw into it afterwards). That buffer's row order is probed once against
    // the HUD-less snapshot: only the HUD may differ in the right orientation. A
    // vertically symmetric frame (a uniform fade, a loading screen) cannot decide it,
    // so the probe repeats on later frames, with HUD handling off until it decides.
    bool SnapshotFinal(bool sourceRegion){
        if(sourceRegion){
            D3D11_BOX box{0,0,0,finalGame.width,finalGame.height,1};
            context11->CopySubresourceRegion(finalGame.texture.Get(),0,0,0,0,producer.Get(),0,&box);
            return true;
        }
        if(!gameBuffer)return false;
        if(gameBuffer.Get()!=probedBuffer || gameBufferFloat!=probedFloat){
            ConvertInto(finalGame,gameBuffer.Get(),true,gameBufferFloat);const UINT64 flipped=Differences(finalGame.view.Get());
            ConvertInto(finalGame,gameBuffer.Get(),false,gameBufferFloat);const UINT64 direct=Differences(finalGame.view.Get());
            const double area=static_cast<double>(finalGame.width)*finalGame.height;
            const UINT64 low=std::min(flipped,direct),high=std::max(flipped,direct);
            if(high==UINT64_MAX || static_cast<double>(high-low)<=area/200 || low*4>=high){
                if(inconclusiveBuffer!=gameBuffer.Get()){
                    inconclusiveBuffer=gameBuffer.Get();
                    Log("FRAME_GEN_GAME_BUFFER buffer=%p row order undecided (flipped=%.2f%% unflipped=%.2f%% differing; symmetric or uniform frame); probing again on later frames",
                        gameBuffer.Get(),flipped==UINT64_MAX?-1.0:100.0*flipped/area,direct==UINT64_MAX?-1.0:100.0*direct/area);
                }
                return false;
            }
            probedBuffer=gameBuffer.Get();probedFloat=gameBufferFloat;gameBufferBottomUp=flipped<direct;
            gameBufferUsable=low<area/2;
            Log(gameBufferUsable?ModLogLevel::Info:ModLogLevel::Warning,"FRAME_GEN_GAME_BUFFER buffer=%p format=%s size=%ux%u rows=%s; pixels differing from the HUD-less image: flipped=%.2f%% unflipped=%.2f%% (the HUD's share is expected)%s",
                probedBuffer,gameBufferFloat?"RGBA16F (sRGB-encoded)":"RGBA8",finalGame.width,finalGame.height,gameBufferBottomUp?"bottom-up":"top-down",
                flipped==UINT64_MAX?-1.0:100.0*flipped/area,direct==UINT64_MAX?-1.0:100.0*direct/area,gameBufferUsable?"":"; neither matches, HUD handling stays off");
            if(gameBufferUsable && !gameBufferBottomUp)return true; // finalGame already holds the unflipped conversion.
        }
        if(!gameBufferUsable)return false;
        ConvertInto(finalGame,gameBuffer.Get(),gameBufferBottomUp,gameBufferFloat);
        return true;
    }
    // UI alpha 1 wherever final differs from the HUD-less snapshot, 0 elsewhere.
    void MaskAlpha(ID3D11ShaderResourceView* final,ID3D11RenderTargetView* target,UINT width,UINT height){
        LayerShaders();
        const float clear[4]{};context11->ClearRenderTargetView(target,clear);
        ID3D11ShaderResourceView* inputs[2]{final,hudless.view.Get()};
        Draw(maskAlphaPixel.Get(),inputs,2,target,width,height);
    }
    // The HUD over the HUD-less image, as premultiplied color: exact with measured
    // HUD opacity, otherwise every changed pixel as opaque UI. The final image is
    // read from a copy, never through a producer view.
    void ComposeUi(BridgeSlot& slot,ID3D11Texture2D* hudOpacity,ID3D11Texture2D* finalTexture,ID3D11ShaderResourceView* finalView,
        ID3D11RenderTargetView* layerTarget,UINT width,UINT height){
        if(layerCovering && !coverageDumped && LogEnabled(ModLogLevel::Debug))DumpCoverage(finalTexture);
        LayerShaders();
        auto opacityInput=OpacityView(hudOpacity);
        const bool measure=!hudOpacity;
        if(measure){
            coverageArea=static_cast<double>(width)*height;
            ReadCoverage(slot);
            if(!slot.coverage){D3D11_QUERY_DESC q{D3D11_QUERY_OCCLUSION,0};RequireBridge(device11->CreateQuery(&q,&slot.coverage),"UI layer coverage query");}
            const float clear[4]{};context11->ClearRenderTargetView(layerTarget,clear); // The mask discards unchanged pixels.
        }
        const bool counting=measure && !slot.coveragePending;
        ID3D11ShaderResourceView* inputs[3]{finalView,hudless.view.Get(),opacityInput};
        Draw(hudOpacity?layerPixel.Get():maskPixel.Get(),inputs,3,layerTarget,width,height,counting?slot.coverage.Get():nullptr);
        if(counting)slot.coveragePending=true;
    }
    // Profiling timestamps. False when the timestamp resources cannot be created.
    UINT64 timestampFrequency=0;
    bool Timing(BridgeSlot& slot){
        if(slot.timeHeap)return true;
        try{
            if(!timestampFrequency)RequireBridge(queue->GetTimestampFrequency(&timestampFrequency),"Profiler timestamp frequency");
            D3D12_QUERY_HEAP_DESC h{};h.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;h.Count=3;
            RequireBridge(device12->CreateQueryHeap(&h,IID_PPV_ARGS(&slot.timeHeap)),"Profiler query heap");
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=3*sizeof(UINT64);buffer.Height=1;
            buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            RequireBridge(device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&slot.timeReadback)),"Profiler readback");
            RequireBridge(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&slot.timeAllocator)),"Profiler allocator");
            RequireBridge(device12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,slot.timeAllocator.Get(),nullptr,IID_PPV_ARGS(&slot.timeList)),"Profiler list");
            RequireBridge(slot.timeList->Close(),"Profiler list close");
            return true;
        }catch(const std::exception&){slot.timeHeap.Reset();slot.timeReadback.Reset();slot.timeAllocator.Reset();slot.timeList.Reset();return false;}
    }
    // After the slot's fence completed: copy-list time and the presenting-queue span after it.
    void ReadTiming(BridgeSlot& slot){
        if(!slot.timed)return;
        slot.timed=false;
        UINT64* ticks{};D3D12_RANGE range{0,3*sizeof(UINT64)};
        if(!timestampFrequency || FAILED(slot.timeReadback->Map(0,&range,reinterpret_cast<void**>(&ticks))))return;
        const UINT64 a=ticks[0],b=ticks[1],c=ticks[2];
        D3D12_RANGE none{0,0};slot.timeReadback->Unmap(0,&none);
        const double scale=1000.0/static_cast<double>(timestampFrequency);
        if(b>=a)ProfilerGpu(PROFILE_LABEL("fg.bridge D3D12 copy list (backbuffer + FG input snapshots)"),static_cast<double>(b-a)*scale);
        if(c>=b)ProfilerGpu(PROFILE_LABEL("fg.presenting-queue span over Present (FG runtime GPU work + any queue idle)"),static_cast<double>(c-b)*scale);
    }
    void ReleaseBuffers(){
        opacity.reset();opacityView.Reset();opacityViewSource=nullptr;
        hudless.Reset();finalGame.Reset();layerGame.Reset();rows.Reset();rowsView.Reset();floatRows.Reset();floatRowsView.Reset();
        gameBuffer.Reset();probedBuffer=nullptr;
        layerCovering=false;coverageHigh=coverageLow=coverageSamples=0;coverageSum=coverageMax=0;uiRecomposing=false;
        producer.Reset();views.Reset();hasImage=false;for(auto& slot:slots)slot=BridgeSlot{};
    }
};
// No mutex is held while DXGI presents or while a GPU completion event waits.
// Concurrent Present/Resize is explicitly rejected rather than risking a window-thread deadlock.
struct BridgeOperation {
    std::atomic<bool>& busy;bool acquired;
    explicit BridgeOperation(std::atomic<bool>& value):busy(value),acquired(!busy.exchange(true)){}
    ~BridgeOperation(){if(acquired)busy=false;}
};
class FrameBridge;
static FrameBridge* gameplayBridge=nullptr; // Only used from Unity's serialized render callbacks.
static std::mutex gameplayBridgeMutex;
class FrameBridge final : public IDXGISwapChain2 {
    std::atomic<ULONG> refs{1};
    std::atomic<bool> busy{false};
    std::mutex bufferMutex;
    std::unique_ptr<BridgeGpu> gpu;
    ComPtr<IDXGIFactory2> factory;
    DWORD presentThread=0;
    bool failed=false;
    UINT64 frames=0;
    LARGE_INTEGER clockFrequency{};
    double cpuMilliseconds=0;
    CameraFrame packet{};
    ComPtr<ID3D11Texture2D> frameDepth,frameMotion;
    int beforeHudFrame=0,lastFgFrame=0;
    int displayFrame=0;
    const void* hudlessSource=nullptr; // Logging only; never dereferenced.
    std::string hudlessRejected;      // Logging only.
    bool hudlessExact=false;          // This frame's HUD-less snapshot is what the HUD drew onto.
    UINT gameWidth=0,gameHeight=0;    // Unity's game resolution this frame (the upscaler output).
    std::atomic<UINT> sourceWidth{0},sourceHeight{0}; // SetSourceSize; 0 when unset.
    std::string mapping;              // Logging only.
    std::string gameBufferRejected;   // Logging only.
    const char* uiFallbackReason=nullptr;
    ~FrameBridge(){
        {std::lock_guard<std::mutex> guard(gameplayBridgeMutex);if(gameplayBridge==this)gameplayBridge=nullptr;}
        if(gpu->fg){FrameGenerationOff("chain retirement");FrameGenerationClearTags(lastFgFrame,gpu->desc.Width,gpu->desc.Height);}
        Log("FRAME_BRIDGE_RETIRED frames=%llu copied=RGBA8 native-renderer=D3D11 display=D3D12",frames);
        if(!gpu->Drain()){
            Log("FRAME_BRIDGE_QUARANTINED pending GPU resources retained until process exit");gpu.release();
        }else if(gpu->fg)FrameGenerationReleaseDisplay();
        --active;
    }
    void UnbindProducer(){
        std::array<ComPtr<ID3D11RenderTargetView>,D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> owned;
        ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ComPtr<ID3D11DepthStencilView> depth;
        gpu->context11->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,targets,&depth);
        bool changed=false;
        for(UINT i=0;i<owned.size();++i){
            owned[i].Attach(targets[i]);
            if(!targets[i])continue;
            ComPtr<ID3D11Resource> resource;targets[i]->GetResource(&resource);
            if(resource.Get()==gpu->producer.Get()){targets[i]=nullptr;changed=true;}
        }
        if(changed)gpu->context11->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,targets,depth.Get());
    }
    HRESULT Show(UINT interval,UINT flags,const DXGI_PRESENT_PARAMETERS* parameters,bool present1){
        if(interval>4)return DXGI_ERROR_INVALID_CALL;
        BridgeOperation operation(busy);if(!operation.acquired)return DXGI_ERROR_WAS_STILL_DRAWING;
        if(failed)return DXGI_ERROR_DEVICE_REMOVED;
        if(flags & DXGI_PRESENT_TEST)return present1?gpu->display->Present1(interval,flags,parameters):gpu->display->Present(interval,flags);
        if(presentThread && presentThread!=GetCurrentThreadId())return DXGI_ERROR_INVALID_CALL;
        presentThread=GetCurrentThreadId();
        LARGE_INTEGER started{};QueryPerformanceCounter(&started);
        ProfileCpuScope profileShow(PROFILE_LABEL("fg.bridge Present total (CPU)"));
        try{
            UINT index=gpu->display->GetCurrentBackBufferIndex();auto& slot=gpu->slots.at(index);
            if((flags & DXGI_PRESENT_DO_NOT_WAIT) && gpu->consumed12->GetCompletedValue()<slot.done)return DXGI_ERROR_WAS_STILL_DRAWING;
            {ProfileCpuScope p(PROFILE_LABEL("fg.bridge wait for this slot's previous D3D12 frame (CPU)"));gpu->Wait(slot.done);}
            gpu->ReadTiming(slot);
            const bool timing=ProfilerEnabled() && gpu->Timing(slot);
            {ProfileCpuScope p(PROFILE_LABEL("fg.bridge wait for FG input release (CPU)"));gpu->WaitFg(slot);} // D3D11 is a non-presenting queue: wait on SL's explicit input fence.
            bool inputs=gpu->fg && FrameGenerationWanted() && frameDepth && frameMotion && packet.frame==beforeHudFrame;
            // Unity may render the game smaller than the window and scale it in, centred
            // with bars; FG then runs on that rectangle, and the HUD work stays at game size.
            const bool scaled=inputs && gameWidth && (gameWidth!=gpu->desc.Width || gameHeight!=gpu->desc.Height);
            const bool sourceRegion=scaled && sourceWidth==gameWidth && sourceHeight==gameHeight;
            DisplayRect content{0,0,gpu->desc.Width,gpu->desc.Height};
            if(scaled)content=sourceRegion?DisplayRect{0,0,gameWidth,gameHeight}:GameRect(gameWidth,gameHeight,gpu->desc.Width,gpu->desc.Height);
            ProfilerBegin(PROFILE_LABEL("fg.bridge D3D11 copies (final image + FG inputs + UI layer)"));
            // Scaled HUD work needs Unity's game-sized final image as well.
            bool gameFinal=false;
            if(scaled && hudlessExact){
                try {gameFinal=gpu->SnapshotFinal(sourceRegion);}
                catch(const std::exception& e){Log(ModLogLevel::Warning,"FRAME_GEN_GAME_FINAL_FAILED %s; HUD handling off this frame",e.what());}
            }
            const bool hudlessUsable=hudlessExact && (!scaled || gameFinal);
            if(inputs){
                char text[256];
                sprintf_s(text,"game=%ux%u chain=%ux%u rect=%u,%u %ux%u mode=%s finalImage=%s",gameWidth,gameHeight,gpu->desc.Width,gpu->desc.Height,
                    content.left,content.top,content.width,content.height,!scaled?"same size":sourceRegion?"SetSourceSize region":"Unity scales the game image (aspect fit, centred)",
                    !scaled?"producer":gameFinal?(sourceRegion?"producer region":"Unity game buffer"):hudlessExact?"unavailable; HUD-less input and UI layer off":"no HUD-less snapshot");
                if(mapping!=text){mapping=text;Log(scaled?ModLogLevel::Info:ModLogLevel::Debug,"FRAME_GEN_DISPLAY_MAPPING %s",text);}
            }
            // FSR interpolates the HUD-less image and composites the HUD from a separate layer.
            bool layered=inputs && hudlessUsable && !gpu->layerUnavailable && FrameGenerationLayersUi();
            // A missing HUD completion must never publish a stale alpha texture.
            auto hudOpacity=gpu->opacity && !gpu->opacityUnavailable?gpu->opacity->Texture():nullptr;
            bool ui=inputs && hudlessUsable && !layered && hudOpacity;
            slot.uiCopied=false;
            D3D11_TEXTURE2D_DESC render{};if(inputs)frameDepth->GetDesc(&render);
            // The presented real frame: Unity's own, or (scaled, with HUD work) the game image
            // placed the same way as the HUD-less image and layer, so they match.
            if(gameFinal)gpu->Place(gpu->finalGame.view.Get(),gpu->SlotTarget(slot.sharedTarget,slot.shared11.Get()),content);
            else gpu->context11->CopyResource(slot.shared11.Get(),gpu->producer.Get());
            if(inputs){
                gpu->Inputs(slot,render.Width,render.Height,layered);
                gpu->context11->CopyResource(slot.depth11.Get(),frameDepth.Get());
                gpu->context11->CopyResource(slot.motion11.Get(),frameMotion.Get());
                if(hudlessUsable){
                    if(scaled)gpu->Place(gpu->hudless.view.Get(),gpu->SlotTarget(slot.hudlessTarget,slot.hudless11.Get()),content);
                    else gpu->context11->CopyResource(slot.hudless11.Get(),gpu->hudless.texture.Get());
                }
                ui=ui && slot.ui11;
                if(ui){
                    if(scaled)gpu->Place(gpu->OpacityView(hudOpacity),gpu->SlotTarget(slot.uiTarget,slot.ui11.Get()),content);
                    else gpu->context11->CopyResource(slot.ui11.Get(),hudOpacity);
                }else if(gpu->uiRecomposing && hudlessUsable && !layered && slot.ui11){
                    // No valid HUD opacity this frame: every pixel the HUD changed is opaque UI.
                    try {
                        auto target=gpu->SlotTarget(slot.uiTarget,slot.ui11.Get());
                        if(scaled){
                            gpu->MaskAlpha(gpu->finalGame.view.Get(),gpu->layerGame.target.Get(),gameWidth,gameHeight);
                            gpu->Place(gpu->layerGame.view.Get(),target,content);
                        }else{
                            if(!slot.finalView)RequireBridge(gpu->device11->CreateShaderResourceView(slot.shared11.Get(),nullptr,&slot.finalView),"UI layer final view");
                            gpu->MaskAlpha(slot.finalView.Get(),target,gpu->desc.Width,gpu->desc.Height);
                        }
                        ui=true;
                    }
                    catch(const std::exception& e){Log(ModLogLevel::Warning,"FRAME_GEN_UI_MASK_FAILED %s; UI recomposition turns off",e.what());}
                }
                slot.uiCopied=ui;
                if(layered){
                    try {
                        if(!slot.layerView)RequireBridge(gpu->device11->CreateRenderTargetView(slot.layer11.Get(),nullptr,&slot.layerView),"UI layer target");
                        if(scaled){
                            gpu->ComposeUi(slot,hudOpacity,gpu->finalGame.texture.Get(),gpu->finalGame.view.Get(),gpu->layerGame.target.Get(),gameWidth,gameHeight);
                            gpu->Place(gpu->layerGame.view.Get(),slot.layerView.Get(),content);
                        }else{
                            if(!slot.finalView)RequireBridge(gpu->device11->CreateShaderResourceView(slot.shared11.Get(),nullptr,&slot.finalView),"UI layer final view");
                            gpu->ComposeUi(slot,hudOpacity,slot.shared11.Get(),slot.finalView.Get(),slot.layerView.Get(),gpu->desc.Width,gpu->desc.Height);
                        }
                    }
                    catch(const std::exception& e){layered=false;gpu->layerUnavailable=true;Log(ModLogLevel::Warning,"FRAME_GEN_UI_LAYER_FAILED %s; FSR HUD-less detection retained",e.what());}
                }
            }
            gpu->gameBuffer.Reset(); // Unity's buffer is referenced only within its frame.
            const bool covering=layered && !hudOpacity && gpu->layerCovering;
            if(covering)layered=false;
            if(gpu->fg){
                layered=FrameGenerationPresentUi(layered?slot.layer12.Get():nullptr);
                int source=layered?(hudOpacity?1:0):covering?2:-1;
                if(source!=gpu->layerSource && gpu->layerLogs<32){
                    ++gpu->layerLogs;
                    Log("FRAME_GEN_UI_LAYER unityFrame=%d source=%s",packet.frame,source==1?"measured HUD opacity":source==0?"HUD difference mask (opaque)":source==2?"off; difference mask covers the screen, FSR HUD-less detection":"off; final image presented");
                }
                gpu->layerSource=source;
            }
            ProfilerEnd(PROFILE_LABEL("fg.bridge D3D11 copies (final image + FG inputs + UI layer)"));
            ProfilerPresent(gpu->device11.Get(),true); // Closes this D3D11 frame for the profiler.
            RequireBridge(slot.allocator->Reset(),"Allocator reset");RequireBridge(slot.list->Reset(slot.allocator.Get(),nullptr),"List reset");
            if(timing)slot.list->EndQuery(slot.timeHeap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
            D3D12_RESOURCE_BARRIER barriers[2]{};
            for(auto& b:barriers){b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;}
            barriers[0].Transition.pResource=slot.shared12.Get();barriers[0].Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON;barriers[0].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
            barriers[1].Transition.pResource=slot.backbuffer.Get();barriers[1].Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT;barriers[1].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
            slot.list->ResourceBarrier(2,barriers);
            if(!layered)slot.list->CopyResource(slot.backbuffer.Get(),slot.shared12.Get());
            if(inputs){
                // Only presenting-queue-owned snapshots are tagged. D3D11 never
                // modifies a tagged resource, even when FG skips interpolation.
                ID3D12Resource* shared[]{slot.depth12.Get(),slot.motion12.Get(),slot.hudless12.Get(),slot.ui12.Get()};
                ID3D12Resource* tagged[]{slot.depthTag12.Get(),slot.motionTag12.Get(),slot.hudlessTag12.Get(),slot.uiTag12.Get()};
                D3D12_RESOURCE_BARRIER copies[8]{};const UINT count=ui?4:3;
                for(UINT i=0;i<count;++i)for(int j=0;j<2;++j){auto& b=copies[2*i+j];b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;b.Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON;
                    b.Transition.pResource=j?tagged[i]:shared[i];b.Transition.StateAfter=j?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_COPY_SOURCE;}
                slot.list->ResourceBarrier(2*count,copies);for(UINT i=0;i<count;++i)slot.list->CopyResource(tagged[i],shared[i]);
                if(layered)slot.list->CopyResource(slot.backbuffer.Get(),slot.hudless12.Get());
                for(UINT i=0;i<2*count;++i)std::swap(copies[i].Transition.StateBefore,copies[i].Transition.StateAfter);slot.list->ResourceBarrier(2*count,copies);
            }
            for(auto& b:barriers)std::swap(b.Transition.StateBefore,b.Transition.StateAfter);
            slot.list->ResourceBarrier(2,barriers);
            if(timing)slot.list->EndQuery(slot.timeHeap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
            RequireBridge(slot.list->Close(),"Copy list close");
            // Separate producer/consumer fences prevent a later D3D11 signal
            // from falsely completing an earlier D3D12 use of a shared slot.
            UINT64 value=++gpu->serial;
            RequireBridge(gpu->context11->Signal(gpu->produced11.Get(),value),"D3D11 finished-frame signal");gpu->context11->Flush();
            gpu->submitted=true;
            RequireBridge(gpu->queue->Wait(gpu->produced12.Get(),value),"D3D12 wait for finished frame");
            ID3D12CommandList* lists[]{slot.list.Get()};gpu->queue->ExecuteCommandLists(1,lists);
            int fgFrame=displayFrame?displayFrame:(inputs?packet.frame:0);
            inputs=inputs && (!displayFrame || displayFrame==packet.frame);
            if(gpu->fg && fgFrame){
                RiskOfRainUpscaling_FrameGenerationMarker(3,fgFrame);
                FrameGenerationPresentMarker(true,fgFrame);lastFgFrame=fgFrame;
            }
            bool generating=false;
            {
                ProfileCpuScope p(PROFILE_LABEL("fg.prepare: SDK tags/constants (CPU)"));
                generating=inputs && FrameGenerationPrepare(packet,slot.depthTag12.Get(),slot.motionTag12.Get(),hudlessUsable?slot.hudlessTag12.Get():nullptr,ui?slot.uiTag12.Get():nullptr,gpu->desc,interval,render.Width,render.Height,content);
            }
            gpu->uiRecomposing=generating && ui;
            if(gpu->opacity)gpu->opacity->Cancel();
            if(!generating && gpu->fg){FrameGenerationOff("Off, missing scene/HUD frame, or unsupported present");FrameGenerationClearTags(lastFgFrame,gpu->desc.Width,gpu->desc.Height);}
            // Full-color copies preserve HUD and color; do not invent dirty/scroll rectangles.
            HRESULT result;
            {
                ProfileCpuScope p(generating?PROFILE_LABEL("fg.D3D12 Present with FG (CPU, includes runtime pacing)"):PROFILE_LABEL("fg.D3D12 Present without FG (CPU)"));
                result=present1?gpu->display->Present1(interval,flags,parameters):gpu->display->Present(interval,flags);
            }
            if(timing){
                // Queue order: copy list, the runtime's Present work, then this timestamp.
                if(SUCCEEDED(slot.timeAllocator->Reset()) && SUCCEEDED(slot.timeList->Reset(slot.timeAllocator.Get(),nullptr))){
                    slot.timeList->EndQuery(slot.timeHeap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,2);
                    slot.timeList->ResolveQueryData(slot.timeHeap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,3,slot.timeReadback.Get(),0);
                    if(SUCCEEDED(slot.timeList->Close())){ID3D12CommandList* timeLists[]{slot.timeList.Get()};gpu->queue->ExecuteCommandLists(1,timeLists);slot.timed=true;}
                }
            }
            if(gpu->fg){
                if(fgFrame)FrameGenerationPresentMarker(false,fgFrame);
                FrameGenerationPresented state{};
                if(FrameGenerationAfterPresent(state)){
                    slot.fgFence=state.inputsFence;
                    slot.fgDone=state.inputsValue;
                    // A skipped/unfocused first frame may have no processing
                    // fence. Tagged snapshots are changed only on this queue;
                    // eBlockPresentingClientQueue supplies the documented ordering.
                    if(frames<4 || frames%300==0 || (generating && state.frames>1 && frames<20))
                        Log("FRAME_GEN_PRESENT unityFrame=%d requested=%d status=0x%x sdkFrames=%u cumulativeSdkFrames=%d inputFence=%p value=%llu",fgFrame,generating,state.status,state.frames,RiskOfRainUpscaling_FrameGenerationPresentedFrames(),state.inputsFence,state.inputsValue);
                }else if(generating){gpu->unknownFgCompletion=true;throw std::runtime_error("FG completion query failed");}
            }
            packet={};frameDepth.Reset();frameMotion.Reset();beforeHudFrame=0;hudlessExact=false;gameWidth=gameHeight=0; // Never reuse a gameplay packet in menus or on a second Present.
            displayFrame=0;
            RequireBridge(gpu->queue->Signal(gpu->consumed12.Get(),value),"D3D12 slot-retirement signal");slot.done=value;gpu->lastIndex=index;gpu->hasImage=true;
            if(result==DXGI_ERROR_DEVICE_REMOVED || result==DXGI_ERROR_DEVICE_RESET || result==DXGI_ERROR_DEVICE_HUNG){
                failed=true;++runtimeFailures;Log(ModLogLevel::Error, "FRAME_BRIDGE_PRESENT_FAILED native HRESULT=0x%08lx; set [Frame Generation] Enabled=false and restart",result);
            }
            if(SUCCEEDED(result)){
                if(!(flags & DXGI_PRESENT_DO_NOT_SEQUENCE))UnbindProducer();
                ++frames;
                LARGE_INTEGER ended{};QueryPerformanceCounter(&ended);
                cpuMilliseconds+=1000.0*static_cast<double>(ended.QuadPart-started.QuadPart)/clockFrequency.QuadPart;
                if(frames<=4 || frames%300==0)Log("FRAME_BRIDGE_PRESENT count=%llu slot=%u serial=%llu HRESULT=0x%08lx meanCpuMs=%.3f render=%ux%u interpolation=%s multiplier=%d",frames,index,value,result,cpuMilliseconds/static_cast<double>(frames),gpu->desc.Width,gpu->desc.Height,generating?"requested":"OFF",RiskOfRainUpscaling_FrameGenerationMultiplier());
            }
            return result;
        }catch(const std::exception& e){failed=true;++runtimeFailures;Log(ModLogLevel::Error, "FRAME_BRIDGE_PRESENT_FAILED %s; set [Frame Generation] Enabled=false and restart",e.what());return DXGI_ERROR_DEVICE_REMOVED;}
    }
public:
    FrameBridge(IDXGIFactory2* owner,ID3D11Device* device,HWND hwnd,const DXGI_SWAP_CHAIN_DESC1& desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,IDXGIOutput* output):gpu(std::make_unique<BridgeGpu>()),factory(owner){
        gpu->device11=device;RequireBridge(device->QueryInterface(IID_PPV_ARGS(&gpu->device5)),"D3D11.5 interface");
        TrackProducerViews(device);
        ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);RequireBridge(context.As(&gpu->context11),"D3D11.4 context");
        ComPtr<IDXGIDevice> dxgi;RequireBridge(device->QueryInterface(IID_PPV_ARGS(&dxgi)),"D3D11 DXGI interface");
        ComPtr<IDXGIAdapter> adapter;RequireBridge(dxgi->GetAdapter(&adapter),"Renderer adapter");
        BuildingScope buildingScope;
        bool fgStarted=FrameGenerationStart(adapter.Get());
        if(fgStarted && FrameGenerationDevice())gpu->device12=FrameGenerationDevice();
        else RequireBridge(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&gpu->device12)),"Same-adapter D3D12 presenter");
        gpu->fg=fgStarted && FrameGenerationBind(gpu->device12.Get());
        gpu->applicationFlags=desc.Flags;
        // The Frame Generation runtime owns the presenting queue and display swap chain.
        ComPtr<IDXGISwapChain1> display;
        if(gpu->fg)FrameGenerationCreateDisplay(gpu->device12.Get(),owner,hwnd,desc,fullscreen,output,gpu->queue,display);
        else CreateBridgeDisplay(gpu->device12.Get(),owner,hwnd,desc,fullscreen,output,gpu->queue,display);
        RequireBridge(gpu->device12->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&gpu->produced12)),"Producer fence");
        BridgeHandle fenceHandle;RequireBridge(gpu->device12->CreateSharedHandle(gpu->produced12.Get(),nullptr,GENERIC_ALL,nullptr,&fenceHandle.value),"Producer fence handle");
        RequireBridge(gpu->device5->OpenSharedFence(fenceHandle.value,IID_PPV_ARGS(&gpu->produced11)),"D3D11 producer fence");
        RequireBridge(gpu->device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gpu->consumed12)),"Consumer fence");
        gpu->event.value=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!gpu->event.value)throw std::runtime_error("Completion event allocation");
        RequireBridge(display.As(&gpu->display),"D3D12 swap-chain interface");display.Reset();
        gpu->Buffers();QueryPerformanceFrequency(&clockFrequency);++active;
        if(gpu->fg){std::lock_guard<std::mutex> guard(gameplayBridgeMutex);gameplayBridge=this;}
        Log("FRAME_BRIDGE_READY proxy=%p window=%p buffers=%u flags=0x%x size=%ux%u; Unity buffer0=D3D11 typeless RGBA8; display=D3D12; FG runtime=%s",this,hwnd,gpu->desc.BufferCount,gpu->desc.Flags,gpu->desc.Width,gpu->desc.Height,gpu->fg?"loaded, suspended until gameplay":"OFF");
    }
    void DlaaInputs(const CameraFrame& frame,ID3D11Texture2D* depth,ID3D11Texture2D* motion,UINT outputWidth,UINT outputHeight){
        BridgeOperation operation(busy);if(!operation.acquired || failed)return;
        if(!gpu->fg || !FrameGenerationWanted())return;
        ComPtr<ID3D11Device> owner;depth->GetDevice(&owner);if(owner.Get()!=gpu->device11.Get())return;
        D3D11_TEXTURE2D_DESC d{},m{};depth->GetDesc(&d);motion->GetDesc(&m);
        // The upscaler output is Unity's game resolution: the chain's size, or smaller
        // when Unity scales the game into the window.
        if(!outputWidth || !outputHeight){outputWidth=gpu->desc.Width;outputHeight=gpu->desc.Height;}
        if(outputWidth>gpu->desc.Width || outputHeight>gpu->desc.Height)return;
        // DLSS supplies render-sized depth/motion; the HUD-less color is game-sized.
        if(!d.Width || !d.Height || d.Width>outputWidth || d.Height>outputHeight || m.Width!=d.Width || m.Height!=d.Height)return;
        packet=frame;frameDepth=depth;frameMotion=motion;gameWidth=outputWidth;gameHeight=outputHeight;
    }
    // The HUD camera has an image effect, so Unity can draw it into an intermediate
    // instead of the producer; lower-resolution DLSS composes the world there. The
    // render target bound at the HUD's BeforeForwardOpaque event is exactly what
    // the HUD draws onto. Use it when it is a game-sized RGBA8 or RGBA16F surface
    // (floatTarget). Otherwise returns null and describes the target: the producer
    // then still holds the previous frame, which must never stand in for this
    // frame's HUD-less image.
    ID3D11Texture2D* HudTarget(ComPtr<ID3D11Texture2D>& bound,std::string& rejected,bool& floatTarget){
        const UINT width=gameWidth?gameWidth:gpu->desc.Width,height=gameHeight?gameHeight:gpu->desc.Height;
        floatTarget=false;
        ComPtr<ID3D11RenderTargetView> view;gpu->context11->OMGetRenderTargets(1,&view,nullptr);
        ComPtr<ID3D11Resource> resource;if(view)view->GetResource(&resource);
        if(!resource || FAILED(resource.As(&bound))){rejected="no 2D render target bound";return nullptr;}
        if(bound.Get()==gpu->producer.Get())return width==gpu->desc.Width && height==gpu->desc.Height?bound.Get():(rejected="producer bound while Unity scales the game",nullptr);
        D3D11_TEXTURE2D_DESC d{};bound->GetDesc(&d);
        bool rgba8=d.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS || d.Format==DXGI_FORMAT_R8G8B8A8_UNORM || d.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        floatTarget=d.Format==DXGI_FORMAT_R16G16B16A16_TYPELESS || d.Format==DXGI_FORMAT_R16G16B16A16_FLOAT;
        if(!(rgba8 || floatTarget) || d.Width!=width || d.Height!=height || d.SampleDesc.Count!=1 || d.ArraySize!=1 || d.MipLevels!=1){
            char text[160];sprintf_s(text,"format=%u size=%ux%u samples=%u arraySize=%u mips=%u",d.Format,d.Width,d.Height,d.SampleDesc.Count,d.ArraySize,d.MipLevels);
            rejected=text;return nullptr;
        }
        return bound.Get();
    }
    void BeforeHud(int frame,bool captureUi){
        BridgeOperation operation(busy);if(!operation.acquired)return;
        if(gpu->opacity)gpu->opacity->Cancel();
        captureUi=captureUi && FrameGenerationUIAvailable();
        if(!displayFrame)displayFrame=frame;
        if(failed || !gpu->fg || !FrameGenerationWanted() || !frameDepth || packet.frame!=frame)return;
        ComPtr<ID3D11Texture2D> bound;std::string rejected;bool floatTarget=false;auto source=HudTarget(bound,rejected,floatTarget);
        // Only the swap-chain producer has top-down rows; Unity RenderTextures are bottom-up on D3D.
        const bool bottomUp=source!=gpu->producer.Get();
        if(source!=hudlessSource || rejected!=hudlessRejected){
            hudlessSource=source;hudlessRejected=rejected;
            if(source)Log("FRAME_GEN_HUDLESS_SOURCE unityFrame=%d source=%p producer=%p kind=%s",frame,source,gpu->producer.Get(),!bottomUp?"producer":floatTarget?"bound RGBA16F HUD target (RenderTexture rows flipped, sRGB-encoded like the HUD's final blit)":"bound HUD target (RenderTexture rows flipped to swap-chain order)");
            else Log(ModLogLevel::Warning,"FRAME_GEN_HUDLESS_SOURCE unityFrame=%d kind=unsupported HUD target (%s); no HUD-less input or UI layer: generated frames interpolate the HUD",frame,rejected.c_str());
        }
        hudlessExact=false;beforeHudFrame=frame;
        if(!source)return; // FG still runs from the final image; no HUD opacity capture.
        try {
            ProfileScope p(PROFILE_LABEL("fg.hudless copy (display-sized)"));
            gpu->GameImages(gameWidth?gameWidth:gpu->desc.Width,gameHeight?gameHeight:gpu->desc.Height);
            gpu->ConvertInto(gpu->hudless,source,bottomUp,floatTarget);
        }
        catch(const std::exception& e){Log(ModLogLevel::Warning,"FRAME_GEN_HUDLESS_FAILED %s; Frame Generation skips this frame",e.what());beforeHudFrame=0;return;}
        hudlessExact=true;
        if(captureUi && !gpu->opacity && !gpu->opacityUnavailable){
            try {gpu->opacity=std::make_unique<HudOpacity>(gpu->device11.Get(),gpu->context11.Get(),gpu->hudless.width,gpu->hudless.height);}
            catch(const std::exception& e){gpu->opacityUnavailable=true;Log(ModLogLevel::Warning, "FRAME_GEN_UI_FALLBACK %s; ordinary HUDless FG retained",e.what());}
        }
        if(captureUi && gpu->opacity && !gpu->opacityUnavailable){
            try {gpu->opacity->Begin(source,frame,bottomUp);ProfilerBegin(PROFILE_LABEL("fg.HUD draws + UI opacity replay (includes the game's HUD)"));}
            catch(const std::exception& e){gpu->opacity->Cancel();gpu->opacityUnavailable=true;Log(ModLogLevel::Warning, "FRAME_GEN_UI_FALLBACK %s; ordinary HUDless FG retained",e.what());}
        }
    }
    // While Unity scales a smaller game image into the window, the HUD camera's target
    // after its image effects is Unity's game-sized screen buffer; overlay canvases
    // (menus) and IMGUI draw into it afterwards, and Present reads it (SnapshotFinal).
    // Not at BeforeImageEffects: with an image effect the HUD renders into a temporary,
    // which never receives the overlays.
    void GameBuffer(int frame){
        gpu->gameBuffer.Reset();
        if(failed || !gpu->fg || !gameWidth || packet.frame!=frame || (gameWidth==gpu->desc.Width && gameHeight==gpu->desc.Height))return;
        if(sourceWidth==gameWidth && sourceHeight==gameHeight)return; // The producer's top-left region holds it.
        ComPtr<ID3D11RenderTargetView> view;gpu->context11->OMGetRenderTargets(1,&view,nullptr);
        ComPtr<ID3D11Resource> resource;if(view)view->GetResource(&resource);
        ComPtr<ID3D11Texture2D> bound;std::string rejected;
        if(!resource || FAILED(resource.As(&bound)))rejected="no 2D render target bound";
        else if(bound.Get()==gpu->producer.Get())rejected="the swap chain itself";
        else{
            D3D11_TEXTURE2D_DESC d{};bound->GetDesc(&d);
            const bool rgba8=d.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS || d.Format==DXGI_FORMAT_R8G8B8A8_UNORM || d.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            const bool half=d.Format==DXGI_FORMAT_R16G16B16A16_TYPELESS || d.Format==DXGI_FORMAT_R16G16B16A16_FLOAT;
            if(!(rgba8 || half) || d.Width!=gameWidth || d.Height!=gameHeight || d.SampleDesc.Count!=1 || d.ArraySize!=1 || d.MipLevels!=1){
                char text[160];sprintf_s(text,"format=%u size=%ux%u samples=%u",d.Format,d.Width,d.Height,d.SampleDesc.Count);rejected=text;
            }else{gpu->gameBuffer=bound;gpu->gameBufferFloat=half;}
        }
        if(rejected!=gameBufferRejected){
            gameBufferRejected=rejected;
            if(!rejected.empty())Log(ModLogLevel::Warning,"FRAME_GEN_GAME_BUFFER unityFrame=%d unsupported (%s); while Unity scales the game, HUD-less input and UI layer stay off",frame,rejected.c_str());
        }
    }
    void AfterHud(int frame,bool safe){
        ProfilerEnd(PROFILE_LABEL("fg.HUD draws + UI opacity replay (includes the game's HUD)"));
        BridgeOperation operation(busy);if(!operation.acquired || !gpu->opacity)return;
        bool valid=gpu->opacity->End(frame);
        const char* reason=safe?gpu->opacity->Reason():"HUD image effect or late diagnostic overlay";
        if(!valid || !safe){
            if(uiFallbackReason!=reason){Log(ModLogLevel::Warning, "FRAME_GEN_UI_FALLBACK unityFrame=%d reason=%s; ordinary HUDless FG retained",frame,reason);uiFallbackReason=reason;}
            gpu->opacity->Cancel();
        }else uiFallbackReason=nullptr;
    }
    void GameBufferPoint(int frame){BridgeOperation operation(busy);if(operation.acquired)GameBuffer(frame);}
    void RenderFrame(int frame){BridgeOperation operation(busy);if(operation.acquired)displayFrame=frame;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** output) override{
        if(!output)return E_POINTER;*output=nullptr;
        if(iid==__uuidof(IUnknown)||iid==__uuidof(IDXGIObject)||iid==__uuidof(IDXGIDeviceSubObject)||iid==__uuidof(IDXGISwapChain)||iid==__uuidof(IDXGISwapChain1)||iid==__uuidof(IDXGISwapChain2)){
            *output=static_cast<IDXGISwapChain2*>(this);AddRef();return S_OK;
        }
        // Do not hand out a D3D12 native interface with the wrong game-facing device.
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override{auto count=--refs;if(!count)delete this;return count;}
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID id,UINT size,const void* data) override{return gpu->display->SetPrivateData(id,size,data);}
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID id,const IUnknown* object) override{return gpu->display->SetPrivateDataInterface(id,object);}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID id,UINT* size,void* data) override{return gpu->display->GetPrivateData(id,size,data);}
    HRESULT STDMETHODCALLTYPE GetParent(REFIID iid,void** output) override{return factory->QueryInterface(iid,output);}
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid,void** output) override{return gpu->device11->QueryInterface(iid,output);}
    HRESULT STDMETHODCALLTYPE Present(UINT interval,UINT flags) override{return Show(interval,flags,nullptr,false);}
    HRESULT STDMETHODCALLTYPE Present1(UINT interval,UINT flags,const DXGI_PRESENT_PARAMETERS* parameters) override{
        if(!parameters)return E_INVALIDARG;
        if(parameters->DirtyRectsCount || parameters->pScrollRect || parameters->pScrollOffset)return DXGI_ERROR_UNSUPPORTED;
        return Show(interval,flags,parameters,true);
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT index,REFIID iid,void** output) override{
        if(!output)return E_POINTER;*output=nullptr;
        // Unity's observed D3D11 contract always renders buffer zero. Other logical
        // indices are deliberately rejected rather than returning an unrelated image.
        if(index){Log("FRAME_BRIDGE_BUFFER_UNSUPPORTED index=%u; observed Unity contract requires buffer0",index);return DXGI_ERROR_INVALID_CALL;}
        std::lock_guard<std::mutex> guard(bufferMutex);
        return gpu->producer?gpu->producer->QueryInterface(iid,output):DXGI_ERROR_DEVICE_REMOVED;
    }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen,IDXGIOutput* target) override{
        if(fullscreen){Log("FRAME_BRIDGE_FULLSCREEN_UNSUPPORTED choose windowed/borderless or restart Native");return DXGI_ERROR_UNSUPPORTED;}
        BridgeOperation operation(busy);if(!operation.acquired)return DXGI_ERROR_INVALID_CALL;
        if(gpu->fg){FrameGenerationOff("display mode change");FrameGenerationClearTags(lastFgFrame,gpu->desc.Width,gpu->desc.Height);if(!gpu->Drain())return DXGI_ERROR_DEVICE_REMOVED;}
        return gpu->display->SetFullscreenState(FALSE,target);
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fullscreen,IDXGIOutput** target) override{return gpu->display->GetFullscreenState(fullscreen,target);}
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* desc) override{return gpu->display->GetDesc(desc);}
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count,UINT width,UINT height,DXGI_FORMAT format,UINT flags) override{
        if((count && (count<2 || count>3)) || (format!=DXGI_FORMAT_UNKNOWN && format!=DXGI_FORMAT_R8G8B8A8_UNORM))return DXGI_ERROR_UNSUPPORTED;
        BridgeOperation operation(busy);if(!operation.acquired)return DXGI_ERROR_INVALID_CALL;
        if(failed)return DXGI_ERROR_DEVICE_REMOVED;
        // Match the native requirement to release direct references and bound views.
        gpu->producer->AddRef();ULONG held=gpu->producer->Release();
        if(held>1 || gpu->views->Count()>2)return DXGI_ERROR_INVALID_CALL;
        try{
            if(gpu->fg){FrameGenerationOff("resolution change");FrameGenerationClearTags(lastFgFrame,gpu->desc.Width,gpu->desc.Height);}
            if(!gpu->Drain())throw std::runtime_error("Resize completion timeout");
            for(auto& slot:gpu->slots)gpu->WaitFg(slot);
            std::lock_guard<std::mutex> guard(bufferMutex);
            gpu->producer->AddRef();held=gpu->producer->Release();
            if(held>1 || gpu->views->Count()>2)return DXGI_ERROR_INVALID_CALL;
            gpu->ReleaseBuffers();
            packet={};frameDepth.Reset();frameMotion.Reset();beforeHudFrame=0;hudlessExact=false;gameWidth=gameHeight=0;
            sourceWidth=sourceHeight=0; // A resize restores the full source region.
            HRESULT result=gpu->display->ResizeBuffers(count,width,height,format,flags);
            gpu->Buffers(); // Rebuild at the actual old/new size, even if DXGI rejected the resize.
            Log("FRAME_BRIDGE_RESIZED size=%ux%u count=%u flags=0x%x HRESULT=0x%08lx",gpu->desc.Width,gpu->desc.Height,gpu->desc.BufferCount,gpu->desc.Flags,result);
            return result;
        }catch(const std::exception& e){failed=true;++runtimeFailures;Log(ModLogLevel::Error, "FRAME_BRIDGE_RESIZE_FAILED %s",e.what());return DXGI_ERROR_DEVICE_REMOVED;}
    }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* target) override{
        BridgeOperation operation(busy);if(!operation.acquired)return DXGI_ERROR_INVALID_CALL;
        if(gpu->fg){FrameGenerationOff("target mode change");FrameGenerationClearTags(lastFgFrame,gpu->desc.Width,gpu->desc.Height);if(!gpu->Drain())return DXGI_ERROR_DEVICE_REMOVED;}
        return gpu->display->ResizeTarget(target);
    }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** output) override{return gpu->display->GetContainingOutput(output);}
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* stats) override{return gpu->display->GetFrameStatistics(stats);}
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* count) override{return gpu->display->GetLastPresentCount(count);}
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) override{return gpu->display->GetDesc1(desc);}
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* desc) override{return gpu->display->GetFullscreenDesc(desc);}
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* hwnd) override{return gpu->display->GetHwnd(hwnd);}
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID,void** output) override{if(output)*output=nullptr;return DXGI_ERROR_UNSUPPORTED;}
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override{return FALSE;}
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** output) override{return gpu->display->GetRestrictToOutput(output);}
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* color) override{return gpu->display->SetBackgroundColor(color);}
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* color) override{return gpu->display->GetBackgroundColor(color);}
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION rotation) override{return gpu->display->SetRotation(rotation);}
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* rotation) override{return gpu->display->GetRotation(rotation);}
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT width,UINT height) override{
        HRESULT result=gpu->display->SetSourceSize(width,height);
        Log("FRAME_BRIDGE_SOURCE_SIZE size=%ux%u chain=%ux%u HRESULT=0x%08lx",width,height,gpu->desc.Width,gpu->desc.Height,result);
        if(SUCCEEDED(result)){const bool full=width==gpu->desc.Width && height==gpu->desc.Height;sourceWidth=full?0:width;sourceHeight=full?0:height;}
        return result;
    }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* width,UINT* height) override{return gpu->display->GetSourceSize(width,height);}
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT value) override{
        if(gpu->fg && !(gpu->applicationFlags&DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))return DXGI_ERROR_INVALID_CALL;
        return gpu->display->SetMaximumFrameLatency(value);
    }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* value) override{return gpu->display->GetMaximumFrameLatency(value);}
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override{
        if(gpu->fg && !(gpu->applicationFlags&DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))return nullptr;
        return gpu->display->GetFrameLatencyWaitableObject();
    }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* matrix) override{return gpu->display->SetMatrixTransform(matrix);}
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* matrix) override{return gpu->display->GetMatrixTransform(matrix);}
    HRESULT Readback(void* bytes,unsigned size,int input=0){
        BridgeOperation operation(busy);if(!operation.acquired || !bytes)return E_INVALIDARG;
        if(failed)return DXGI_ERROR_DEVICE_REMOVED;
        try{
            if(!gpu->hasImage || input<0 || input>5)return E_INVALIDARG;
            gpu->Wait(gpu->slots[gpu->lastIndex].done);
            auto& slot=gpu->slots[gpu->lastIndex];gpu->WaitFg(slot);
            if(input==4 && !slot.uiCopied)return E_INVALIDARG;
            ID3D12Resource* source=input==1?slot.depthTag12.Get():input==2?slot.motionTag12.Get():input==3?slot.hudlessTag12.Get():input==4?slot.uiTag12.Get():input==5?slot.layer12.Get():slot.backbuffer.Get();
            if(!source)return E_INVALIDARG;
            auto desc=source->GetDesc();
            const UINT pixelBytes=input==4?1:4;
            const UINT width=static_cast<UINT>(desc.Width),height=desc.Height;
            if(size!=width*height*pixelBytes)return E_INVALIDARG;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 total{};
            gpu->device12->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&total);
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=total;buffer.Height=1;
            buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            // Keep the resource alive even if the test's completion wait times out.
            gpu->pendingReadback.Reset();
            RequireBridge(gpu->device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&gpu->pendingReadback)),"Test-only readback");
            auto readback=gpu->pendingReadback.Get();
            RequireBridge(slot.allocator->Reset(),"Readback allocator reset");RequireBridge(slot.list->Reset(slot.allocator.Get(),nullptr),"Readback list reset");
            D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=source;
            b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;b.Transition.StateBefore=input?D3D12_RESOURCE_STATE_COMMON:D3D12_RESOURCE_STATE_PRESENT;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
            slot.list->ResourceBarrier(1,&b);
            D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=source;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=readback;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
            slot.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);std::swap(b.Transition.StateBefore,b.Transition.StateAfter);slot.list->ResourceBarrier(1,&b);
            RequireBridge(slot.list->Close(),"Readback close");ID3D12CommandList* lists[]{slot.list.Get()};gpu->queue->ExecuteCommandLists(1,lists);
            UINT64 value=++gpu->serial;RequireBridge(gpu->queue->Signal(gpu->consumed12.Get(),value),"Readback completion");slot.done=value;gpu->Wait(value);
            void* mapped{};D3D12_RANGE range{0,static_cast<SIZE_T>(total)};RequireBridge(readback->Map(0,&range,&mapped),"Readback map");
            for(UINT y=0;y<height;++y)std::memcpy(static_cast<unsigned char*>(bytes)+y*width*pixelBytes,static_cast<unsigned char*>(mapped)+footprint.Offset+y*footprint.Footprint.RowPitch,width*pixelBytes);
            D3D12_RANGE noWrites{0,0};readback->Unmap(0,&noWrites);gpu->pendingReadback.Reset();return S_OK;
        }catch(...){failed=true;++runtimeFailures;return E_FAIL;}
    }
};
void CreateBridgeDisplay(ID3D12Device* queueDevice,IDXGIFactory2* factory,HWND hwnd,const DXGI_SWAP_CHAIN_DESC1& desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,IDXGIOutput* output,ComPtr<ID3D12CommandQueue>& queue,ComPtr<IDXGISwapChain1>& display){
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    RequireBridge(queueDevice->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"Display queue");
    BuildingScope scope;RequireBridge(factory->CreateSwapChainForHwnd(queue.Get(),hwnd,&desc,fullscreen,output,&display),"Single D3D12 window swap chain");
}
void FrameBridgeSubmitInputs(const CameraFrame& packet,ID3D11Texture2D* depth,ID3D11Texture2D* motion,UINT outputWidth,UINT outputHeight){std::lock_guard<std::mutex> guard(gameplayBridgeMutex);if(gameplayBridge)gameplayBridge->DlaaInputs(packet,depth,motion,outputWidth,outputHeight);}
void FrameBridgeBeforeHud(int frame,bool captureUi){std::lock_guard<std::mutex> guard(gameplayBridgeMutex);if(gameplayBridge)gameplayBridge->BeforeHud(frame,captureUi);}
void FrameBridgeAfterHud(int frame,bool safe){std::lock_guard<std::mutex> guard(gameplayBridgeMutex);if(gameplayBridge)gameplayBridge->AfterHud(frame,safe);}
void FrameBridgeGameBuffer(int frame){std::lock_guard<std::mutex> guard(gameplayBridgeMutex);if(gameplayBridge)gameplayBridge->GameBufferPoint(frame);}
void FrameBridgeRenderFrame(int frame){std::lock_guard<std::mutex> guard(gameplayBridgeMutex);if(gameplayBridge)gameplayBridge->RenderFrame(frame);}
HRESULT CreateFrameBridge(IDXGIFactory2* factory,ID3D11Device* device,HWND hwnd,const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,IDXGIOutput* output,IDXGISwapChain1** result){
    if(!result)return E_POINTER;*result=nullptr;
    if(!factory || !device || !hwnd || !desc)return E_INVALIDARG;
    if(desc->Format!=DXGI_FORMAT_R8G8B8A8_UNORM || desc->SampleDesc.Count!=1 || desc->SampleDesc.Quality || desc->Stereo ||
       desc->BufferCount<2 || desc->BufferCount>3 || (desc->SwapEffect!=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL && desc->SwapEffect!=DXGI_SWAP_EFFECT_FLIP_DISCARD) ||
       (fullscreen && !fullscreen->Windowed)){++creationFailures;return DXGI_ERROR_UNSUPPORTED;}
    try{*result=new FrameBridge(factory,device,hwnd,*desc,fullscreen,output);selectedWindow=hwnd;return S_OK;}
    catch(const std::exception& e){++creationFailures;Log(ModLogLevel::Error, "FRAME_BRIDGE_CREATE_FAILED %s; native creation fallback permitted before any game-facing chain exists",e.what());return E_FAIL;}
}
HRESULT FrameBridgeReadback(IDXGISwapChain* chain,void* bytes,unsigned size){
    auto bridge=dynamic_cast<FrameBridge*>(chain);return bridge?bridge->Readback(bytes,size):E_NOINTERFACE;
}
HRESULT FrameBridgeInputReadback(IDXGISwapChain* chain,int input,void* bytes,unsigned size){
    auto bridge=dynamic_cast<FrameBridge*>(chain);return bridge?bridge->Readback(bytes,size,input):E_NOINTERFACE;
}
