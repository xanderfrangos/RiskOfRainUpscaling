#include "FrameGeneration/FrameGenerator.h"
#include "FrameGeneration/FrameGeneration.h"
#include "Core/FfxRuntime.h"
#include "Pacing/FramePacer.h"
#include <framegeneration/include/ffx_framegeneration.h>
#include <framegeneration/include/dx12/ffx_api_framegeneration_dx12.h>
#include <array>
#include <mutex>
#include <stdexcept>
#include <string>
#include <cstring>
#include <algorithm>

static void RequireFfx(ffxReturnCode_t code,const char* name){
    if(code!=FFX_API_RETURN_OK){Log(ModLogLevel::Error,"FRAME_GEN_FFX_FAILED %s=%s (%u)",name,FfxResultName(code),code);throw std::runtime_error(name);}
}
static void RequireHr(HRESULT hr,const char* name){
    if(FAILED(hr)){Log(ModLogLevel::Error,"FRAME_GEN_FFX_FAILED %s HRESULT=0x%08lx",name,hr);throw std::runtime_error(name);}
}
// The bridge's tagged snapshots stay in COMMON on the presenting queue.
static FfxApiResource Common(ID3D12Resource* resource){return ffxApiGetResourceDX12(resource,FFX_API_RESOURCE_STATE_COMMON);}

// AMD FSR Frame Generation through the FidelityFX frame-generation swap chain,
// which replaces the bridge's display swap chain. Interpolation runs on the
// presenting queue inside Present (no async compute), so the bridge's
// post-Present fence also covers the tagged depth/motion/HUD-less snapshots.
class FsrFrameGenerator final : public FrameGenerator {
    const FfxRuntime* ffx{};
    std::mutex mutex; // FidelityFX contexts are not thread safe.
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain4> swapChain;
    ffxContext swapChainContext{}, context{};
    UINT contextWidth=0,contextHeight=0;uint32_t contextFlags=0;DXGI_FORMAT contextFormat=DXGI_FORMAT_UNKNOWN;
    bool enabled=false,uiRegistered=false;
    uint64_t lastFrame=0;
    struct Recording {ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;UINT64 done=0;};
    std::array<Recording,4> recordings;
    ComPtr<ID3D12Fence> fence;UINT64 serial=0;HANDLE event{};
    UINT lastPresentCount=0;bool havePresentCount=false;
    LARGE_INTEGER frequency{},lastPrepare{};
    std::mutex pacerMutex;FramePacer capPacer;
    std::atomic<bool> bound{false};
    std::atomic<int> frameLimitUs{0},guardWaits{0};
    static ffxReturnCode_t Generate(ffxDispatchDescFrameGeneration* params,void* user){
        auto self=static_cast<FsrFrameGenerator*>(user);return self->ffx->dispatch(&self->context,&params->header);
    }
    // Configuring Off waits for the swap chain's outstanding interpolation work.
    void DestroyGeneration(){
        if(!context)return;
        ffxConfigureDescFrameGeneration off{};off.header.type=FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        off.swapChain=swapChain.Get();off.frameGenerationEnabled=false;off.frameID=lastFrame;
        auto a=ffx->configure(&context,&off.header);auto b=ffx->destroy(&context,nullptr);
        Log("FRAME_GEN_FSR_CONTEXT_DESTROYED configureOff=%s destroy=%s",FfxResultName(a),FfxResultName(b));
        context=nullptr;enabled=false;
    }
    void CreateGeneration(UINT width,UINT height,uint32_t flags,DXGI_FORMAT format){
        ffxCreateContextDescFrameGeneration create{};create.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        create.flags=flags;create.displaySize={width,height};create.maxRenderSize={width,height};
        create.backBufferFormat=ffxApiGetSurfaceFormatDX12(format);
        ffxCreateContextDescFrameGenerationVersion version{};version.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
        version.version=FFX_FRAMEGENERATION_VERSION;
        ffxCreateBackendDX12Desc backend{};backend.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;backend.device=device.Get();
        create.header.pNext=&version.header;version.header.pNext=&backend.header;
        RequireFfx(ffx->create(&context,&create.header,nullptr),"FSR frame generation context");
        contextWidth=width;contextHeight=height;contextFlags=flags;contextFormat=format;
        ffxQueryGetProviderVersion provider{};provider.header.type=FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        ffx->query(&context,&provider.header);
        Log("FRAME_GEN_FSR_CONTEXT display=%ux%u flags=0x%x provider=%s; UI layer composited by the swap chain; synchronous interpolation on the presenting queue",
            width,height,flags,provider.versionName?provider.versionName:"unknown");
    }
public:
    ~FsrFrameGenerator() override {if(event)CloseHandle(event);}
    void Start(IDXGIAdapter*,int multiplier) override {
        ffx=&LoadFfxRuntime(L"amd_fidelityfx_framegeneration_dx12.dll");
        QueryPerformanceFrequency(&frequency);
        Log("FRAME_GEN_INITIALIZED backend=FSR owned FidelityFX loader before presenter device/queue/swap-chain creation; requested=%dx; FSR generates one frame per rendered frame",multiplier);
    }
    void Bind(ID3D12Device* bindDevice) override {
        device=bindDevice;
        RequireHr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"FSR prepare fence");
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)throw std::runtime_error("FSR prepare event");
        for(auto& r:recordings){
            RequireHr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&r.allocator)),"FSR prepare allocator");
            RequireHr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,r.allocator.Get(),nullptr,IID_PPV_ARGS(&r.list)),"FSR prepare list");
            RequireHr(r.list->Close(),"FSR prepare list close");
        }
        uint64_t count=8;uint64_t ids[8]{};const char* names[8]{};
        ffxQueryDescGetVersions versions{};versions.header.type=FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        versions.createDescType=FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;versions.device=device.Get();
        versions.outputCount=&count;versions.versionIds=ids;versions.versionNames=names;
        std::string list;
        if(ffx->query(nullptr,&versions.header)==FFX_API_RETURN_OK)
            for(uint64_t i=0;i<std::min<uint64_t>(count,8);++i){if(i)list+=", ";list+=names[i]?names[i]:"?";}
        bound=true;
        Log("FRAME_GEN_DEVICE_READY backend=FSR versions=[%s] (first is the default); default FG=Off; native frame-start guard limiter",list.c_str());
    }
    void CreateDisplay(ID3D12Device* queueDevice,IDXGIFactory2* factory,HWND hwnd,const DXGI_SWAP_CHAIN_DESC1& desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,IDXGIOutput*,ComPtr<ID3D12CommandQueue>& displayQueue,ComPtr<IDXGISwapChain1>& display) override {
        std::lock_guard<std::mutex> guard(mutex);
        // DXGI allows one swap chain per window; a retained proxy cannot be replaced.
        if(swapChainContext)throw std::runtime_error("Previous FSR display swap chain still active");
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        RequireHr(queueDevice->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"Display queue");
        displayQueue=queue;
        DXGI_SWAP_CHAIN_DESC1 chainDesc=desc;DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc{};
        if(fullscreen)fullscreenDesc=*fullscreen;
        IDXGISwapChain4* created=nullptr;
        ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 create{};create.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
        create.swapchain=&created;create.hwnd=hwnd;create.desc=&chainDesc;create.fullscreenDesc=fullscreen?&fullscreenDesc:nullptr;
        create.dxgiFactory=factory;create.gameQueue=queue.Get();
        ffxCreateContextDescFrameGenerationSwapChainVersionDX12 version{};version.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
        version.version=FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;create.header.pNext=&version.header;
        RequireFfx(ffx->create(&swapChainContext,&create.header,nullptr),"FSR frame generation swap chain");
        if(!created)throw std::runtime_error("FSR frame generation swap chain null");
        swapChain.Attach(created);
        RequireHr(swapChain.As(&display),"FSR swap-chain interface");
        havePresentCount=false;
        Log("FRAME_GEN_FSR_SWAPCHAIN window=%p buffers=%u size=%ux%u; FidelityFX proxy paces and presents generated frames",hwnd,desc.BufferCount,desc.Width,desc.Height);
    }
    void ReleaseDisplay() override {
        std::lock_guard<std::mutex> guard(mutex);
        DestroyGeneration();
        if(swapChainContext){auto result=ffx->destroy(&swapChainContext,nullptr);swapChainContext=nullptr;Log("FRAME_GEN_FSR_SWAPCHAIN_RELEASED destroy=%s",FfxResultName(result));}
        swapChain.Reset();queue.Reset();havePresentCount=false;uiRegistered=false;
    }
    void Marker(int marker,int) override {
        if(marker)return;
        std::lock_guard<std::mutex> guard(pacerMutex);
        if(capPacer.Wait(FrameGenerationLoaded()?frameLimitUs.load():0) && guardWaits.fetch_add(1)==0)
            Log("FRAME_LIMITER_GUARD_USED backend=FSR native frame-start interval guard active");
    }
    void PresentMarker(bool,int) override {}
    int Prepare(FrameGenerationRequest& request,const char*& offReason) override {
        std::lock_guard<std::mutex> guard(mutex);
        const auto& packet=request.packet;const auto& desc=request.desc;
        if(!swapChain)throw std::runtime_error("FSR display swap chain missing");
        if(!request.renderWidth || !request.renderHeight || request.renderWidth>desc.Width || request.renderHeight>desc.Height){
            offReason="resolution unsupported";return 0;
        }
        // The swap chain composites the registered UI layer; R8 opacity is not an FSR input.
        request.uiAlpha=nullptr;
        const uint32_t flags=packet.reversedDepth?FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED:0;
        if(context && (contextWidth!=desc.Width || contextHeight!=desc.Height || contextFlags!=flags || contextFormat!=desc.Format))DestroyGeneration();
        if(!context)CreateGeneration(desc.Width,desc.Height,flags,desc.Format);
        const bool reset=packet.reset || !enabled;
        const uint64_t frame=static_cast<uint64_t>(packet.frame);
        ffxConfigureDescFrameGeneration config{};config.header.type=FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        config.swapChain=swapChain.Get();config.frameGenerationCallback=&Generate;config.frameGenerationCallbackUserContext=this;
        config.frameGenerationEnabled=true;config.allowAsyncWorkloads=false;
        // With a UI layer the backbuffer is already HUD-less. Otherwise FSR detects
        // the HUD by comparing the HUD-less snapshot with the final frame.
        if(!uiRegistered)config.HUDLessColor=Common(request.hudless);
        config.generationRect={0,0,static_cast<int32_t>(desc.Width),static_cast<int32_t>(desc.Height)};config.frameID=frame;
        enabled=true; // Attempt Off even if an On call partially fails.
        RequireFfx(ffx->configure(&context,&config.header),"FSR frame generation configure");
        lastFrame=frame;
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        float delta=lastPrepare.QuadPart?static_cast<float>(1000.0*(now.QuadPart-lastPrepare.QuadPart)/frequency.QuadPart):16.667f;
        lastPrepare=now;delta=std::clamp(delta,.1f,1000.f);
        auto& recording=recordings[serial%recordings.size()];
        if(fence->GetCompletedValue()<recording.done){
            RequireHr(fence->SetEventOnCompletion(recording.done,event),"FSR prepare completion event");
            if(WaitForSingleObject(event,5000)!=WAIT_OBJECT_0)throw std::runtime_error("FSR prepare completion timeout");
        }
        RequireHr(recording.allocator->Reset(),"FSR prepare allocator reset");
        RequireHr(recording.list->Reset(recording.allocator.Get(),nullptr),"FSR prepare list reset");
        ffxDispatchDescFrameGenerationPrepareV2 prepare{};prepare.header.type=FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        prepare.frameID=frame;prepare.commandList=recording.list.Get();
        prepare.renderSize={request.renderWidth,request.renderHeight};
        // Packet jitter is the projection shift in render pixels (y down), as FSR expects:
        // AMD's sample shifts NDC by (-2jx/w, 2jy/h) and passes (-jx, -jy).
        prepare.jitterOffset={packet.jitterX,packet.jitterY};
        // Packet motion is current-to-previous UV; FSR expects render pixels.
        prepare.motionVectorScale={packet.motionScaleX*request.renderWidth,packet.motionScaleY*request.renderHeight};
        prepare.frameTimeDelta=delta;prepare.reset=reset;
        prepare.cameraNear=packet.nearPlane;prepare.cameraFar=packet.farPlane;prepare.cameraFovAngleVertical=packet.fov;
        prepare.viewSpaceToMetersFactor=0.f;
        prepare.depth=Common(request.depth);prepare.motionVectors=Common(request.motion);
        std::memcpy(prepare.cameraPosition,packet.position,sizeof(prepare.cameraPosition));
        std::memcpy(prepare.cameraUp,packet.up,sizeof(prepare.cameraUp));
        std::memcpy(prepare.cameraRight,packet.right,sizeof(prepare.cameraRight));
        std::memcpy(prepare.cameraForward,packet.forward,sizeof(prepare.cameraForward));
        RequireFfx(ffx->dispatch(&context,&prepare.header),"FSR frame generation prepare");
        RequireHr(recording.list->Close(),"FSR prepare list close");
        ID3D12CommandList* lists[]{recording.list.Get()};queue->ExecuteCommandLists(1,lists);
        RequireHr(queue->Signal(fence.Get(),++serial),"FSR prepare signal");recording.done=serial;
        if(!FrameGenerationLoaded()){offReason="asynchronous API failure";return 0;}
        return 2;
    }
    bool Off(const char*& error) override {
        std::lock_guard<std::mutex> guard(mutex);
        if(!enabled || !context)return false;
        ffxConfigureDescFrameGeneration off{};off.header.type=FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        off.swapChain=swapChain.Get();off.frameGenerationEnabled=false;off.frameID=lastFrame;
        if(ffx->configure(&context,&off.header)!=FFX_API_RETURN_OK){error="FSR frame generation Off failed";return false;}
        enabled=false;return true;
    }
    void ClearTags(int,unsigned,unsigned) override {}
    bool LayersUi() const override {return true;}
    // The swap chain composites the layer onto real and generated frames, also while
    // generation is off, and copies it at Present (internal double buffering).
    void PresentUi(ID3D12Resource* layer) override {
        std::lock_guard<std::mutex> guard(mutex);
        if(!swapChainContext || (!layer && !uiRegistered))return;
        ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 ui{};
        ui.header.type=FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
        if(layer)ui.uiResource=Common(layer);
        ui.flags=FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA|FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING;
        uiRegistered=false; // Present the final image if registration fails.
        RequireFfx(ffx->configure(&swapChainContext,&ui.header),"FSR UI layer");
        uiRegistered=layer!=nullptr;
    }
    const char* AfterPresent(FrameGenerationPresented& presented) override {
        std::lock_guard<std::mutex> guard(mutex);
        if(!swapChain)return "FSR display swap chain missing";
        // Counts every present the proxy issued on the real swap chain, generated or rendered.
        UINT count=0;
        if(SUCCEEDED(swapChain->GetLastPresentCount(&count))){
            if(havePresentCount)presented.frames=count-lastPresentCount;
            lastPresentCount=count;havePresentCount=true;
        }
        return nullptr;
    }
    unsigned MaxGenerated() const override {return 1;}
    int FrameLimit() const override {return bound?frameLimitUs.load():-1;}
    int SetFrameLimit(int limit) override {
        if(!bound)return -1;
        std::lock_guard<std::mutex> guard(pacerMutex);
        if(limit && !FrameGenerationLoaded())return -1;
        if(frameLimitUs==limit)return limit;
        frameLimitUs=limit;capPacer.Reset();
        Log("FRAME_LIMITER_OPTIONS backend=FSR frameLimitUs=%d; native frame-start interval guard (no Reflex)",limit);return limit;
    }
    int GuardWaits() const override {return guardWaits;}
};
std::unique_ptr<FrameGenerator> CreateFsrFrameGenerator(){return std::make_unique<FsrFrameGenerator>();}
