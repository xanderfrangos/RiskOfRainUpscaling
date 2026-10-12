#include "FrameGeneration/FrameGenerator.h"
#include "FrameGeneration/FrameGeneration.h"
#include "FrameGeneration/FrameGenerationMode.h"
#include "FrameGeneration/FrameBridge.h"
#include "Pacing/FramePacer.h"
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <sl_helpers.h>
#include <filesystem>
#include <tlhelp32.h>
#include <stdexcept>
#include <mutex>
#include <map>
#include <cstring>

static void RequireF(sl::Result result,const char* name) {
    if(result!=sl::Result::eOk){Log(ModLogLevel::Error, "FRAME_GEN_API_FAILED %s=%s (%d)",name,sl::getResultAsStr(result),static_cast<int>(result));throw std::runtime_error(name);}
}
static void FLog(sl::LogType type,const char* message) {
    Log(type == sl::LogType::eError ? ModLogLevel::Error : type == sl::LogType::eWarn ? ModLogLevel::Warning : ModLogLevel::Debug,
        "FRAME_GEN_STREAMLINE level=%d %s",static_cast<int>(type),message?message:"");
}
static void FApiError(const sl::APIError& error){if(FAILED(error.hres)){Log(ModLogLevel::Error, "FRAME_GEN_ASYNC_PRESENT_ERROR HRESULT=0x%08lx",error.hres);FrameGenerationFail("Asynchronous DXGI presentation failed");}}

// DLSS Frame Generation, Reflex and PCL through an owned D3D12 Streamline
// instance, independent from the D3D11 DLAA instance. Never unload another
// component's SDK.
class DlssFrameGenerator final : public FrameGenerator {
    HMODULE module{};
    bool enabled=false;
    bool haveCapabilities=false;
    uint32_t minDimension=0,maxGenerated=0;
    bool vsync=false;
    bool dynamicSupported=false,dynamicRejected=false; // bIsDynamicMFGSupported; latched after the SDK refused eDynamic
    unsigned width=0,height=0;
    std::wstring logs; // The SDK keeps this path for the process lifetime.
    PFun_slInit* init{}; PFun_slSetD3DDevice* bind{};
    PFun_slIsFeatureSupported* supported{}; PFun_slGetFeatureFunction* function{};
    PFun_slUpgradeInterface* upgrade{}; PFun_slGetNewFrameToken* token{};
    PFun_slSetConstants* constants{}; PFun_slSetTagForFrame* tag{};
    PFun_slDLSSGSetOptions* options{}; PFun_slDLSSGGetState* getState{};
    PFun_slReflexSetOptions* reflexOptions{}; PFun_slReflexGetState* reflexState{};
    PFun_slReflexSleep* sleep{}; PFun_slPCLSetMarker* marker{};
    std::mutex tokenMutex;
    mutable std::mutex reflexMutex; // SetOptions/GetState are not thread safe.
    std::atomic<bool> reflexReady{false};
    std::atomic<int> frameLimitUs{0};
    std::atomic<int> guardWaits{0};
    FramePacer capPacer;
    ULONGLONG healthAt{};
    struct Timing {sl::FrameToken* token{};unsigned mask{};};
    std::map<int,Timing> timings;
    sl::ViewportHandle viewport{0};
    template<class T> T Proc(const char* name) {
        auto p=reinterpret_cast<T>(GetProcAddress(module,name));if(!p)throw std::runtime_error(name);return p;
    }
    template<class T> T Feature(sl::Feature feature,const char* name) {
        void* p{};if(function(feature,name,p)!=sl::Result::eOk || !p)throw std::runtime_error(name);return reinterpret_cast<T>(p);
    }
    sl::FrameToken* Token(int frame){
        auto found=timings.find(frame);if(found!=timings.end())return found->second.token;
        if(frame<=0)return nullptr;
        uint32_t index=static_cast<uint32_t>(frame);sl::FrameToken* result{};RequireF(token(result,&index),"FG frame token");
        if(!result)throw std::runtime_error("FG frame token null");
        // SDK tokens have bounded lifetime; limit queued CPU frame bookkeeping too.
        while(timings.size()>=16)timings.erase(timings.begin());
        timings.emplace(frame,Timing{result,0});return result;
    }
    void GuardFrameCap(){
        std::lock_guard<std::mutex> guard(reflexMutex);
        // A floor at the same frame-start seam covers driver/bridge combinations
        // that accept frameLimitUs but do not enforce it. Reflex always sleeps first.
        if(capPacer.Wait(FrameGenerationLoaded()?frameLimitUs.load():0)){
            if(guardWaits.fetch_add(1)==0)Log("REFLEX_LIMITER_GUARD_USED driver sleep did not cover the requested interval; native frame-start floor active");
        }
    }
public:
    void Start(IDXGIAdapter* adapter,int multiplier) override {
        auto directory=g_runtimeFolder+L"\\D3D12";
        HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,GetCurrentProcessId());
        if(snapshot==INVALID_HANDLE_VALUE)throw std::runtime_error("Vendor ownership audit unavailable");
        MODULEENTRY32W item{};item.dwSize=sizeof(item);bool foreign=false;
        if(Module32FirstW(snapshot,&item))do{
            if(!_wcsnicmp(item.szModule,L"sl.",3) || !_wcsnicmp(item.szModule,L"nvngx",5)){
                auto prefix=g_runtimeFolder+L"\\";
                if(_wcsnicmp(item.szExePath,prefix.c_str(),prefix.size())){Log(ModLogLevel::Warning, "FRAME_GEN_FOREIGN_RUNTIME %ls",item.szExePath);foreign=true;}
            }
        }while(Module32NextW(snapshot,&item));
        CloseHandle(snapshot);if(foreign)throw std::runtime_error("Foreign NVIDIA runtime owns the process");
        auto path=directory+L"\\sl.interposer.dll";
        if(!VerifyNvidiaSignature(path.c_str()))throw std::runtime_error("Interposer signature invalid");
        module=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!module)throw std::runtime_error("Owned D3D12 runtime load failed");
        wchar_t actual[32768]{};GetModuleFileNameW(module,actual,32768);
        if(_wcsicmp(actual,path.c_str()))throw std::runtime_error("Foreign D3D12 interposer path");
        init=Proc<PFun_slInit*>("slInit");bind=Proc<PFun_slSetD3DDevice*>("slSetD3DDevice");
        supported=Proc<PFun_slIsFeatureSupported*>("slIsFeatureSupported");function=Proc<PFun_slGetFeatureFunction*>("slGetFeatureFunction");
        upgrade=Proc<PFun_slUpgradeInterface*>("slUpgradeInterface");token=Proc<PFun_slGetNewFrameToken*>("slGetNewFrameToken");
        constants=Proc<PFun_slSetConstants*>("slSetConstants");tag=Proc<PFun_slSetTagForFrame*>("slSetTagForFrame");
        logs=g_logFolder+L"\\FrameGeneration";std::filesystem::create_directories(logs);
        const wchar_t* paths[]{directory.c_str()};const sl::Feature features[]{sl::kFeatureDLSS_G,sl::kFeatureReflex,sl::kFeaturePCL};
        sl::Preferences prefs{};prefs.pathsToPlugins=paths;prefs.numPathsToPlugins=1;prefs.featuresToLoad=features;prefs.numFeaturesToLoad=3;
        prefs.renderAPI=sl::RenderAPI::eD3D12;prefs.engine=sl::EngineType::eUnity;prefs.engineVersion="2021.3.33";
        prefs.projectId="b1ac70ad-8304-406a-8b07-0a45519cfeae";prefs.pathToLogsAndData=logs.c_str();prefs.logMessageCallback=&FLog;
        prefs.flags=sl::PreferenceFlags::eUseManualHooking|sl::PreferenceFlags::eDisableCLStateTracking|sl::PreferenceFlags::eUseFrameBasedResourceTagging;
        RequireF(init(prefs,sl::kSDKVersion),"D3D12 slInit");
        DXGI_ADAPTER_DESC desc{};if(FAILED(adapter->GetDesc(&desc)))throw std::runtime_error("Adapter description");
        sl::AdapterInfo info{};info.deviceLUID=reinterpret_cast<uint8_t*>(&desc.AdapterLuid);info.deviceLUIDSizeInBytes=sizeof(LUID);
        for(auto feature:features)RequireF(supported(feature,info),"DLSSG/Reflex/PCL support");
        Log("FRAME_GEN_INITIALIZED owned D3D12 SDK before presenter device/queue/swap-chain creation; viewport=0; requested=%dx",multiplier);
    }
    void Bind(ID3D12Device* device) override {
        RequireF(bind(device),"D3D12 device binding");
        options=Feature<PFun_slDLSSGSetOptions*>(sl::kFeatureDLSS_G,"slDLSSGSetOptions");
        getState=Feature<PFun_slDLSSGGetState*>(sl::kFeatureDLSS_G,"slDLSSGGetState");
        reflexOptions=Feature<PFun_slReflexSetOptions*>(sl::kFeatureReflex,"slReflexSetOptions");
        reflexState=Feature<PFun_slReflexGetState*>(sl::kFeatureReflex,"slReflexGetState");
        sleep=Feature<PFun_slReflexSleep*>(sl::kFeatureReflex,"slReflexSleep");
        marker=Feature<PFun_slPCLSetMarker*>(sl::kFeaturePCL,"slPCLSetMarker");
        std::lock_guard<std::mutex> guard(reflexMutex);
        sl::ReflexOptions reflex{};reflex.mode=sl::ReflexMode::eLowLatency;RequireF(reflexOptions(reflex),"Reflex Low Latency");
        sl::ReflexState state{};RequireF(reflexState(state),"Reflex availability");
        if(!state.lowLatencyAvailable)throw std::runtime_error("Reflex Low Latency unavailable");
        reflexReady=true;
        Log("FRAME_GEN_DEVICE_READY Reflex=LowLatency; default FG=Off; D3D11 SDK owns no Reflex pacing");
    }
    void CreateDisplay(ID3D12Device* device,IDXGIFactory2* factory,HWND hwnd,const DXGI_SWAP_CHAIN_DESC1& desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,IDXGIOutput* output,ComPtr<ID3D12CommandQueue>& queue,ComPtr<IDXGISwapChain1>& display) override {
        // The SDK presents through proxies of the queue's device and the display factory.
        ComPtr<ID3D12Device> queueDevice;ComPtr<IDXGIFactory2> displayFactory;
        void* upgraded=device;auto r=upgrade(&upgraded);
        if(r!=sl::Result::eOk || upgraded==device)throw std::runtime_error("D3D12 SDK device upgrade failed");
        queueDevice.Attach(static_cast<ID3D12Device*>(upgraded));
        upgraded=factory;r=upgrade(&upgraded);
        if(r!=sl::Result::eOk || upgraded==factory)throw std::runtime_error("D3D12 SDK factory upgrade failed");
        displayFactory.Attach(static_cast<IDXGIFactory2*>(upgraded));
        CreateBridgeDisplay(queueDevice.Get(),displayFactory.Get(),hwnd,desc,fullscreen,output,queue,display);
    }
    void Marker(int index,int frame) override {
        sl::FrameToken* current{};unsigned bit=1u<<index;
        {std::lock_guard<std::mutex> guard(tokenMutex);current=Token(frame);if(!current || (timings.at(frame).mask&bit))return;}
        // Begin each Unity frame before its input/update work. Unity keeps the
        // requested DXGI waitable handle; SL's pacer never consumes that handle.
        if(index==0){
            RequireF(sleep(*current),"Reflex frame-start sleep");
            if(GetTickCount64()-healthAt>=1000){
                std::lock_guard<std::mutex> guard(reflexMutex);
                sl::ReflexState state{};RequireF(reflexState(state),"Reflex pacing health");
                if(!state.lowLatencyAvailable)throw std::runtime_error("Reflex sleep became unavailable");
                healthAt=GetTickCount64();
            }
            GuardFrameCap();
        }
        RequireF(marker(static_cast<sl::PCLMarker>(index),*current),"Unity timing marker");
        {std::lock_guard<std::mutex> guard(tokenMutex);auto found=timings.find(frame);if(found!=timings.end() && found->second.token==current)found->second.mask|=bit;}
    }
    void PresentMarker(bool start,int frame) override {
        sl::FrameToken* current{};{std::lock_guard<std::mutex> guard(tokenMutex);current=Token(frame);}
        RequireF(marker(start?sl::PCLMarker::ePresentStart:sl::PCLMarker::ePresentEnd,*current),"FG present marker");
    }
    bool Off(const char*& error) override {
        if(!enabled)return false;
        sl::DLSSGOptions off{};off.mode=sl::DLSSGMode::eOff;
        if(options(viewport,off)!=sl::Result::eOk){error="DLSSG Off failed";return false;}
        enabled=false;return true;
    }
    void ClearTags(int frame,unsigned w,unsigned h) override {
        std::lock_guard<std::mutex> guard(tokenMutex);auto current=Token(frame);
        if(w && h){width=w;height=h;}
        sl::Extent extent{};extent.width=width;extent.height=height;
        sl::ResourceTag tags[]{{nullptr,sl::kBufferTypeDepth,sl::ResourceLifecycle::eValidUntilPresent},
            {nullptr,sl::kBufferTypeMotionVectors,sl::ResourceLifecycle::eValidUntilPresent},
            {nullptr,sl::kBufferTypeHUDLessColor,sl::ResourceLifecycle::eValidUntilPresent},
            {nullptr,sl::kBufferTypeUIAlpha,sl::ResourceLifecycle::eValidUntilPresent},
            {nullptr,sl::kBufferTypeBackbuffer,sl::ResourceLifecycle::eValidUntilPresent,&extent}};
        RequireF(tag(*current,viewport,tags,5,nullptr),"Clear FG tags");
    }
    int Prepare(FrameGenerationRequest& request,const char*& offReason) override {
        const auto& packet=request.packet;const auto& desc=request.desc;
        std::lock_guard<std::mutex> guard(tokenMutex);
        auto found=timings.find(packet.frame);
        if(found==timings.end() || (found->second.mask&7)!=7){offReason="missing Unity simulation/render markers";return 0;}
        auto current=found->second.token;
        width=desc.Width;height=desc.Height;
        if(!haveCapabilities)return 0;
        if(desc.Width<minDimension || desc.Height<minDimension || maxGenerated<1 || (request.interval && !vsync) ||
           !request.renderWidth || !request.renderHeight || request.renderWidth>desc.Width || request.renderHeight>desc.Height){
            offReason="resolution or VSync unsupported";return 0;
        }
        const auto choice=SelectFrameGenerationMode(request.dynamic && !dynamicRejected,dynamicSupported,request.multiplier,maxGenerated);
        const int multiplier=choice.multiplier;
        sl::Constants c{};
        std::memcpy(&c.cameraViewToClip[0].x,packet.projection,sizeof(packet.projection));
        std::memcpy(&c.clipToCameraView[0].x,packet.inverseProjection,sizeof(packet.inverseProjection));
        std::memcpy(&c.clipToPrevClip[0].x,packet.clipToPrevious,sizeof(packet.clipToPrevious));
        std::memcpy(&c.prevClipToClip[0].x,packet.previousToClip,sizeof(packet.previousToClip));
        c.jitterOffset={packet.jitterX,packet.jitterY};c.mvecScale={packet.motionScaleX,packet.motionScaleY};c.cameraPinholeOffset={0,0};
        c.cameraPos={packet.position[0],packet.position[1],packet.position[2]};c.cameraUp={packet.up[0],packet.up[1],packet.up[2]};
        c.cameraRight={packet.right[0],packet.right[1],packet.right[2]};c.cameraFwd={packet.forward[0],packet.forward[1],packet.forward[2]};
        c.cameraNear=packet.nearPlane;c.cameraFar=packet.farPlane;c.cameraFOV=packet.fov;c.cameraAspectRatio=packet.aspect;
        c.depthInverted=packet.reversedDepth?sl::Boolean::eTrue:sl::Boolean::eFalse;c.cameraMotionIncluded=sl::Boolean::eTrue;
        c.reset=(packet.reset || !enabled)?sl::Boolean::eTrue:sl::Boolean::eFalse;c.motionVectors3D=sl::Boolean::eFalse;
        c.motionVectorsJittered=c.motionVectorsDilated=sl::Boolean::eFalse;
        RequireF(constants(c,*current,viewport),"FG common constants");
        // Backbuffer subrect: the game image Unity scales into the chain. HUD-less and UI
        // alpha are display-sized with the same extent; outside it SL copies the backbuffer.
        sl::Extent extent{};extent.left=request.content.left;extent.top=request.content.top;extent.width=request.content.width;extent.height=request.content.height;
        sl::Extent render{};render.width=request.renderWidth;render.height=request.renderHeight;
        sl::Resource d{sl::ResourceType::eTex2d,request.depth,nullptr,nullptr,D3D12_RESOURCE_STATE_COMMON};
        sl::Resource m{sl::ResourceType::eTex2d,request.motion,nullptr,nullptr,D3D12_RESOURCE_STATE_COMMON};
        sl::Resource h{sl::ResourceType::eTex2d,request.hudless,nullptr,nullptr,D3D12_RESOURCE_STATE_COMMON};
        sl::Resource u{sl::ResourceType::eTex2d,request.uiAlpha,nullptr,nullptr,D3D12_RESOURCE_STATE_COMMON};
        sl::ResourceTag tags[]{{&d,sl::kBufferTypeDepth,sl::ResourceLifecycle::eValidUntilPresent,&render},
            {&m,sl::kBufferTypeMotionVectors,sl::ResourceLifecycle::eValidUntilPresent,&render},
            {request.hudless?&h:nullptr,sl::kBufferTypeHUDLessColor,sl::ResourceLifecycle::eValidUntilPresent,&extent}, // Null: no HUD-less image this frame.
            {request.uiAlpha?&u:nullptr,sl::kBufferTypeUIAlpha,sl::ResourceLifecycle::eValidUntilPresent,&extent},
            {nullptr,sl::kBufferTypeBackbuffer,sl::ResourceLifecycle::eValidUntilPresent,&extent}};
        auto tagResult=tag(*current,viewport,tags,5,nullptr);
        if(tagResult!=sl::Result::eOk && request.uiAlpha){
            Log(ModLogLevel::Warning, "FRAME_GEN_UI_FALLBACK SDK UI tag rejected: %s (%d); retrying ordinary HUDless inputs",sl::getResultAsStr(tagResult),static_cast<int>(tagResult));
            request.uiRejected=true;request.uiAlpha=nullptr;tags[3].resource=nullptr;
            tagResult=tag(*current,viewport,tags,5,nullptr);
        }
        RequireF(tagResult,"FG frame resource tags");
        // Dynamic ignores numFramesToGenerate; it stays the highest supported count so the options remain valid.
        sl::DLSSGOptions on{};on.mode=choice.dynamic?sl::DLSSGMode::eDynamic:sl::DLSSGMode::eOn;on.numFramesToGenerate=multiplier-1;
        on.dynamicTargetFrameRate=choice.dynamic?request.dynamicTargetFps:0.0f; // 0: the display refresh rate
        // Fixed-ratio DLSS: mvec/depth size is constant per quality mode, so the
        // dynamic-resolution flag stays clear (DLSS-G guide section 10).
        on.numBackBuffers=desc.BufferCount;on.colorWidth=desc.Width;on.colorHeight=desc.Height;
        on.mvecDepthWidth=request.renderWidth;on.mvecDepthHeight=request.renderHeight;on.colorBufferFormat=desc.Format;
        on.depthBufferFormat=DXGI_FORMAT_R32_FLOAT;on.mvecBufferFormat=DXGI_FORMAT_R16G16_FLOAT;
        on.hudLessBufferFormat=DXGI_FORMAT_R8G8B8A8_UNORM;
        on.enableUserInterfaceRecomposition=request.uiAlpha?sl::Boolean::eTrue:sl::Boolean::eFalse;
        on.onErrorCallback=&FApiError;on.queueParallelismMode=sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
        enabled=true; // Attempt Off even if an On call partially fails.
        auto optionResult=options(viewport,on);
        if(optionResult!=sl::Result::eOk && on.mode==sl::DLSSGMode::eDynamic){
            Log(ModLogLevel::Warning, "FRAME_GEN_DYNAMIC_FALLBACK SDK Dynamic options rejected: %s (%d); retrying fixed %dx",sl::getResultAsStr(optionResult),static_cast<int>(optionResult),multiplier);
            dynamicRejected=request.dynamicRejected=true;on.mode=sl::DLSSGMode::eOn;on.dynamicTargetFrameRate=0.0f;
            optionResult=options(viewport,on);
        }
        if(optionResult!=sl::Result::eOk && request.uiAlpha){
            Log(ModLogLevel::Warning, "FRAME_GEN_UI_FALLBACK SDK recomposition options rejected: %s (%d); retrying ordinary HUDless FG",sl::getResultAsStr(optionResult),static_cast<int>(optionResult));
            request.uiRejected=true;request.uiAlpha=nullptr;
            sl::ResourceTag clearUi{nullptr,sl::kBufferTypeUIAlpha,sl::ResourceLifecycle::eValidUntilPresent};
            RequireF(tag(*current,viewport,&clearUi,1,nullptr),"Clear rejected UI tag");
            sl::DLSSGOptions off{};off.mode=sl::DLSSGMode::eOff;RequireF(options(viewport,off),"DLSSG UI fallback Off");
            on.enableUserInterfaceRecomposition=sl::Boolean::eFalse;
            optionResult=options(viewport,on);
        }
        RequireF(optionResult,on.mode==sl::DLSSGMode::eDynamic?"DLSSG dynamic options":"DLSSG fixed-multiplier options");
        request.dynamicActive=on.mode==sl::DLSSGMode::eDynamic;
        if(!FrameGenerationLoaded()){offReason="asynchronous API failure";return 0;}
        return multiplier;
    }
    const char* AfterPresent(FrameGenerationPresented& presented) override {
        sl::DLSSGState state{};
        if(getState(viewport,state,nullptr)!=sl::Result::eOk)return "DLSSG post-Present state query failed";
        if(!haveCapabilities)Log("FRAME_GEN_CAPABILITIES minDimension=%u maxGenerated=%u VSync=%d dynamic=%d",state.minWidthOrHeight,state.numFramesToGenerateMax,state.bIsVsyncSupportAvailable==sl::Boolean::eTrue,state.bIsDynamicMFGSupported==sl::Boolean::eTrue);
        minDimension=state.minWidthOrHeight;maxGenerated=state.numFramesToGenerateMax;vsync=state.bIsVsyncSupportAvailable==sl::Boolean::eTrue;
        dynamicSupported=state.bIsDynamicMFGSupported==sl::Boolean::eTrue;haveCapabilities=true;
        presented.frames=state.numFramesActuallyPresented;
        presented.inputsFence=static_cast<ID3D12Fence*>(state.inputsProcessingCompletionFence);
        presented.inputsValue=state.lastPresentInputsProcessingCompletionFenceValue;
        presented.status=static_cast<unsigned>(state.status);
        if(enabled && state.status!=sl::DLSSGStatus::eOk)presented.failure="DLSSG status failure";
        return nullptr;
    }
    unsigned MaxGenerated() const override {return maxGenerated;}
    int FrameLimit() const override {return reflexReady?frameLimitUs.load():-1;}
    int SetFrameLimit(int limit) override {
        if(!reflexReady)return -1;
        std::lock_guard<std::mutex> guard(reflexMutex);
        if(limit && !FrameGenerationLoaded())return -1;
        if(frameLimitUs==limit)return limit;
        sl::ReflexOptions reflex{};reflex.mode=sl::ReflexMode::eLowLatency;reflex.frameLimitUs=static_cast<uint32_t>(limit);
        auto result=reflexOptions(reflex);
        if(result!=sl::Result::eOk){
            Log(ModLogLevel::Error, "REFLEX_LIMITER_FAILED requestedUs=%d result=%s; attempting cap clear",limit,sl::getResultAsStr(result));
            reflex.frameLimitUs=0;auto cleared=reflexOptions(reflex);
            capPacer.Reset();
            if(cleared==sl::Result::eOk)frameLimitUs=0;
            else Log(ModLogLevel::Error, "REFLEX_LIMITER_CLEAR_FAILED result=%s",sl::getResultAsStr(cleared));
            FrameGenerationFail("Reflex limiter options failed");return -1;
        }
        frameLimitUs=limit;capPacer.Reset();
        Log("REFLEX_LIMITER_OPTIONS frameLimitUs=%d mode=LowLatency; native interval guard armed when capped",limit);return limit;
    }
    int GuardWaits() const override {return guardWaits;}
};
std::unique_ptr<FrameGenerator> CreateDlssFrameGenerator(){return std::make_unique<DlssFrameGenerator>();}
