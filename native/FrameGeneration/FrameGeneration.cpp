#include "FrameGeneration/FrameGeneration.h"
#include "FrameGeneration/FrameBridge.h"
#include <algorithm>
#include <climits>
#include <stdexcept>

// Backend-independent Frame Generation state. The backend owns its SDK; this
// facade owns the user's request, failure latching and exported state.
struct FrameGenerationRuntime {
    std::unique_ptr<FrameGenerator> backend;
    std::unique_ptr<FrameGenerator> declined; // An Auto candidate that failed to start keeps its SDK state.
    FrameGenerationBackend preference=FrameGenerationBackend::Auto;
    std::atomic<int> kind{0};
    std::atomic<bool> requested{false}, wanted{false}, ready{false}, failed{false};
    std::atomic<int> state{0}, presented{0};
    std::atomic<int> requestedMultiplier{2};
    std::atomic<int> multiplier{2};
    int appliedRequestedMultiplier=0;
    bool started=false, bound=false;
    bool uiRecomposition=false;
    std::atomic<bool> uiRequestFailed{false};
    ComPtr<ID3D12Device> device;
};
static auto f=new FrameGenerationRuntime(); // Process resident; no SDK calls during static destruction.
void ConfigureFrameGeneration(bool enabled,int multiplier,FrameGenerationBackend backend){
    if(!f->started)f->preference=backend;
    f->requestedMultiplier=std::clamp(multiplier,2,6);f->multiplier=f->requestedMultiplier.load();
    f->requested=enabled;f->wanted=enabled;f->state=f->failed?-2:(enabled?1:0);
}
bool FrameGenerationLoaded(){return f->ready && !f->failed;}
ID3D12Device* FrameGenerationDevice(){return FrameGenerationLoaded()?f->device.Get():nullptr;}
bool FrameGenerationWanted(){return FrameGenerationLoaded() && f->wanted;}
bool FrameGenerationUIAvailable(){return !f->uiRequestFailed.load();}
void FrameGenerationFail(const char* reason){f->failed=true;f->state=-2;Log(ModLogLevel::Error, "FRAME_GEN_FAILED %s; interpolation latched OFF; ordinary bridge retained",reason);}
extern "C" int __cdecl RiskOfRainUpscaling_FrameGenerationState(){return f->state;}
extern "C" int __cdecl RiskOfRainUpscaling_FrameGenerationPresentedFrames(){return f->presented;}
extern "C" int __cdecl RiskOfRainUpscaling_FrameGenerationMultiplier(){return f->requested?f->multiplier.load():0;}
extern "C" int __cdecl RiskOfRainUpscaling_FrameGenerationKind(){return f->kind;}
extern "C" int __cdecl RiskOfRainUpscaling_FrameGenerationSetMultiplier(int multiplier){
    if(multiplier<2 || multiplier>6 || !f->requested || f->failed)return -1;
    // The main thread only publishes a request. SDK calls stay before Present.
    f->requestedMultiplier=multiplier;return multiplier;
}
extern "C" void __cdecl RiskOfRainUpscaling_FrameGenerationSetEnabled(int enabled){f->wanted=enabled!=0 && f->requested && !f->failed;}
extern "C" int __cdecl RiskOfRainUpscaling_ReflexFrameLimit(){return f->backend && !f->failed?f->backend->FrameLimit():-1;}
extern "C" int __cdecl RiskOfRainUpscaling_ReflexGuardWaits(){return f->backend?f->backend->GuardWaits():0;}
extern "C" int __cdecl RiskOfRainUpscaling_ReflexSetFrameLimit(int limit){
    if(limit<0 || limit>1000000 || !f->backend)return -1;
    return f->backend->SetFrameLimit(limit);
}

static bool Unavailable(const char* reason,UINT vendor){
    f->failed=true;f->state=-2;
    Log(ModLogLevel::Warning, "FRAME_GEN_UNAVAILABLE %s (adapter vendor 0x%04x); interpolation OFF; ordinary bridge retained",reason,vendor);
    return false;
}
// Backends retain their SDK state even if Start fails: an initialized SDK keeps references into it.
static void StartBackend(FrameGenerationBackend kind,IDXGIAdapter* adapter){
    f->backend=kind==FrameGenerationBackend::DLSS?CreateDlssFrameGenerator():CreateFsrFrameGenerator();
    f->backend->Start(adapter,f->requestedMultiplier.load());
    f->kind=static_cast<int>(kind);
}
bool FrameGenerationStart(IDXGIAdapter* adapter){
    if(!f->requested || f->failed)return false;
    if(f->started)return FrameGenerationLoaded();
    f->started=true;
    try {
        ComPtr<IDXGIAdapter1> adapter1;DXGI_ADAPTER_DESC1 desc{};
        if(FAILED(adapter->QueryInterface(IID_PPV_ARGS(&adapter1))) || FAILED(adapter1->GetDesc1(&desc)))throw std::runtime_error("Adapter description");
        if(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)return Unavailable("software adapter",desc.VendorId);
        const bool nvidia=desc.VendorId==NvidiaVendorId;
        auto kind=f->preference;
        // Never load NVIDIA runtimes for another vendor's adapter.
        if(kind==FrameGenerationBackend::DLSS && !nvidia)return Unavailable("DLSS Frame Generation requires an NVIDIA adapter; select Auto or FSR",desc.VendorId);
        if(kind==FrameGenerationBackend::Auto && nvidia){
            try {StartBackend(FrameGenerationBackend::DLSS,adapter);}
            catch(const std::exception& e){
                Log(ModLogLevel::Warning, "FRAME_GEN_DLSSG_UNAVAILABLE %s; Auto selects FSR Frame Generation",e.what());
                f->declined=std::move(f->backend);
            }
        }
        if(!f->backend)StartBackend(kind==FrameGenerationBackend::DLSS?FrameGenerationBackend::DLSS:FrameGenerationBackend::FSR,adapter);
        f->ready=true;f->state=2;
        Log("FRAME_GEN_BACKEND selected=%s preference=%s vendor=0x%04x",f->kind==1?"DLSS":"FSR",
            f->preference==FrameGenerationBackend::Auto?"Auto":f->preference==FrameGenerationBackend::DLSS?"DLSS":"FSR",desc.VendorId);
        return true;
    }catch(const std::exception& e){FrameGenerationFail(e.what());return false;}
}
bool FrameGenerationBind(ID3D12Device* device){
    if(!FrameGenerationLoaded())return false;
    try {
        if(f->device && f->device.Get()!=device)throw std::runtime_error("Presenter device changed; restart required");
        if(f->device.Get()==device && f->bound)return true;
        f->device=device;f->backend->Bind(device);f->bound=true;return true;
    }catch(const std::exception& e){FrameGenerationFail(e.what());return false;}
}
void FrameGenerationCreateDisplay(ID3D12Device* device,IDXGIFactory2* factory,HWND hwnd,const DXGI_SWAP_CHAIN_DESC1& desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,IDXGIOutput* output,ComPtr<ID3D12CommandQueue>& queue,ComPtr<IDXGISwapChain1>& display){
    if(!f->bound)throw std::runtime_error("Frame Generation runtime not bound");
    f->backend->CreateDisplay(device,factory,hwnd,desc,fullscreen,output,queue,display);
}
void FrameGenerationReleaseDisplay(){
    if(!f->bound)return;
    try {f->backend->ReleaseDisplay();}catch(const std::exception& e){FrameGenerationFail(e.what());}
}
extern "C" void __cdecl RiskOfRainUpscaling_FrameGenerationMarker(int marker,int frame){
    if(!FrameGenerationLoaded() || !f->bound || marker<0 || marker>3)return;
    try {f->backend->Marker(marker,frame);}catch(const std::exception& e){FrameGenerationFail(e.what());}
}
void FrameGenerationPresentMarker(bool start,int frame){
    if(!FrameGenerationLoaded() || frame<=0)return;
    try {f->backend->PresentMarker(start,frame);}catch(const std::exception& e){FrameGenerationFail(e.what());}
}
void FrameGenerationOff(const char* reason){
    if(!f->bound)return;
    const char* error=nullptr;
    if(f->backend->Off(error))Log("FRAME_GEN_SUSPENDED reason=%s",reason);
    else if(error){FrameGenerationFail(error);return;}
    if(!f->failed)f->state=f->wanted?2:0;
}
void FrameGenerationClearTags(int frame,unsigned width,unsigned height){
    if(!f->ready || frame<=0)return;
    try {f->backend->ClearTags(frame,width,height);}catch(const std::exception& e){FrameGenerationFail(e.what());}
}
bool FrameGenerationLayersUi(){return FrameGenerationLoaded() && f->bound && f->backend->LayersUi();}
bool FrameGenerationPresentUi(ID3D12Resource* layer){
    if(!f->bound)return false;
    if(f->failed)layer=nullptr; // Still unregister a previous layer.
    try {f->backend->PresentUi(layer);return layer!=nullptr;}
    catch(const std::exception& e){
        if(!f->failed){FrameGenerationFail(e.what());FrameGenerationOff("UI layer failure");}
        return false;
    }
}
bool FrameGenerationPrepare(const CameraFrame& packet,ID3D12Resource* depth,ID3D12Resource* motion,ID3D12Resource* hudless,ID3D12Resource* uiAlpha,const DXGI_SWAP_CHAIN_DESC1& desc,UINT interval,UINT renderWidth,UINT renderHeight,const DisplayRect& content){
    if(!FrameGenerationWanted())return false;
    if(!FrameGenerationUIAvailable())uiAlpha=nullptr;
    const int requestedMultiplier=f->requestedMultiplier.load();
    FrameGenerationRequest request{packet,depth,motion,hudless,uiAlpha,false,desc,interval,renderWidth,renderHeight,requestedMultiplier,content};
    try {
        const char* offReason=nullptr;
        const int multiplier=f->backend->Prepare(request,offReason);
        if(request.uiRejected)f->uiRequestFailed=true;
        if(!multiplier){if(offReason)FrameGenerationOff(offReason);return false;}
        if(f->state.load()!=3 || f->appliedRequestedMultiplier!=requestedMultiplier || f->multiplier.load()!=multiplier){
            Log("FRAME_GEN_MULTIPLIER requested=%dx supportedMax=%ux selected=%dx%s",requestedMultiplier,std::min(f->backend->MaxGenerated(),5u)+1,multiplier,multiplier<requestedMultiplier?"; requested mode unsupported; using highest supported mode":"");
            Log("FRAME_GEN_ENABLED unityFrame=%d generatedPerRender=%u multiplier=%dx requested=%dx viewport=0 size=%ux%u mvecDepth=%ux%u HUDless=exact pre-UI producer; UI %s",packet.frame,static_cast<unsigned>(multiplier-1),multiplier,requestedMultiplier,desc.Width,desc.Height,renderWidth,renderHeight,
                f->backend->LayersUi()?"layer composited by the swap chain":request.uiAlpha?"recomposition=ON":"recomposition=OFF");
        }
        if(!f->backend->LayersUi() && f->uiRecomposition!=(request.uiAlpha!=nullptr))Log("FRAME_GEN_UI_RECOMPOSITION unityFrame=%d enabled=%d UIAlpha=%s",packet.frame,request.uiAlpha!=nullptr,request.uiAlpha?"R8 display-size current-frame HUD draws":"null; ordinary HUDless FG retained");
        f->uiRecomposition=request.uiAlpha!=nullptr;
        f->multiplier=multiplier;f->appliedRequestedMultiplier=requestedMultiplier;
        f->state=3;return true;
    }catch(const std::exception& e){
        if(request.uiRejected)f->uiRequestFailed=true;
        FrameGenerationFail(e.what());FrameGenerationOff("API failure");return false;
    }
}
bool FrameGenerationAfterPresent(FrameGenerationPresented& state){
    if(!f->bound)return false;
    if(auto error=f->backend->AfterPresent(state)){FrameGenerationFail(error);FrameGenerationOff("state query");return false;}
    f->presented.fetch_add(static_cast<int>(state.frames));
    if(state.failure){Log(ModLogLevel::Error, "FRAME_GEN_STATUS_FAILED bits=0x%x",state.status);FrameGenerationFail(state.failure);FrameGenerationOff("status failure");}
    return true;
}
static void __stdcall FEvent(int frame,void* skipUi){if(frame>0)FrameBridgeBeforeHud(frame,skipUi==nullptr);else if(frame<0 && frame!=INT_MIN)FrameBridgeRenderFrame(-frame);}
extern "C" void* __cdecl RiskOfRainUpscaling_FrameGenerationGetBeforeHudEvent(){return reinterpret_cast<void*>(&FEvent);}
// Separate callback: signed frame specifies whether managed post-effects and
// late overlays are safe. Existing negative FEvent IDs keep their frame role.
static void __stdcall FHudEvent(int frame,void*){if(frame && frame!=INT_MIN)FrameBridgeAfterHud(frame>0?frame:-frame,frame>0);}
extern "C" void* __cdecl RiskOfRainUpscaling_FrameGenerationGetAfterHudEvent(){return reinterpret_cast<void*>(&FHudEvent);}
