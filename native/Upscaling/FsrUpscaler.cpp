#include "Upscaling/Upscaler.h"
#include "Core/FfxRuntime.h"
#include <api/include/dx12/ffx_api_dx12.h>
#include <upscalers/include/ffx_upscale.h>
#include <array>
#include <algorithm>
#include <stdexcept>
#include <string>

static void RequireHr(HRESULT hr, const char* label) {
    if (FAILED(hr)) { Log(ModLogLevel::Error, "FSR_FAILED %s HRESULT=0x%08lx", label, hr); throw std::runtime_error(label); }
}
static void RequireFfx(ffxReturnCode_t code, const char* label) {
    if (code != FFX_API_RETURN_OK) { Log(ModLogLevel::Error, "FSR_FAILED %s=%s (%u)", label, FfxResultName(code), code); throw std::runtime_error(label); }
}
static void UpscaleMessage(uint32_t type, const wchar_t* message) {
    Log(type==FFX_API_MESSAGE_TYPE_ERROR?ModLogLevel::Error:ModLogLevel::Warning, "FSR_MESSAGE type=%u %ls", type, message?message:L"");
}
static uint32_t QualityMode(int quality) {
    switch(quality) {
    case 1: return FFX_UPSCALE_QUALITY_MODE_QUALITY;
    case 2: return FFX_UPSCALE_QUALITY_MODE_BALANCED;
    case 3: return FFX_UPSCALE_QUALITY_MODE_PERFORMANCE;
    case 4: return FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE;
    default: return FFX_UPSCALE_QUALITY_MODE_NATIVEAA;
    }
}

// AMD FSR upscaling for Unity's D3D11 frame. The FidelityFX API is D3D12-only,
// so each evaluation crosses devices on the same adapter: D3D11 copies the
// inputs into shared textures and signals a shared fence; a private D3D12 queue
// waits, runs FSR and signals; D3D11 waits on the GPU and copies the result back.
class FsrFeature final : public UpscalerFeature {
    struct Shared { ComPtr<ID3D12Resource> d3d12; ComPtr<ID3D11Texture2D> d3d11; };
    struct Recording { ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list; UINT64 done=0; };
    const FfxRuntime* ffx{};
    ComPtr<ID3D11Device1> device11;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence12;
    ComPtr<ID3D11Fence> fence11;
    UINT64 fenceValue=0;
    HANDLE event{};
    std::array<Recording,3> recordings;
    size_t recordingIndex=0;
    Shared color, depth, motion, output;
    UINT renderWidth=0, renderHeight=0, outputWidth=0, outputHeight=0;
    ffxContext context{};
    uint32_t contextFlags=0;
    UpscalerOptions current{};
    LARGE_INTEGER frequency{}, last{};
    std::string versions, defaultVersion;

    // Created on D3D11 with an NT handle and opened on D3D12, as the presentation
    // bridge shares its inputs. D3D12 sees it in COMMON.
    Shared Create(DXGI_FORMAT format, UINT width, UINT height, bool unordered) {
        D3D11_TEXTURE2D_DESC d{}; d.Width=width; d.Height=height; d.MipLevels=d.ArraySize=1; d.Format=format; d.SampleDesc.Count=1;
        d.BindFlags=D3D11_BIND_SHADER_RESOURCE|(unordered?D3D11_BIND_UNORDERED_ACCESS:D3D11_BIND_RENDER_TARGET);
        d.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        Shared shared;
        RequireHr(device11->CreateTexture2D(&d,nullptr,&shared.d3d11),"FSR shared texture");
        ComPtr<IDXGIResource1> resource; RequireHr(shared.d3d11.As(&resource),"FSR shared resource interface");
        HANDLE handle{};
        RequireHr(resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle),"FSR shared texture handle");
        HRESULT opened=device12->OpenSharedHandle(handle,IID_PPV_ARGS(&shared.d3d12)); CloseHandle(handle);
        RequireHr(opened,"FSR shared texture on D3D12");
        if(unordered && !(shared.d3d12->GetDesc().Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) throw std::runtime_error("FSR shared output lacks D3D12 unordered access");
        return shared;
    }
    static DXGI_FORMAT Format(ID3D11Texture2D* texture) { D3D11_TEXTURE2D_DESC d{}; texture->GetDesc(&d); return d.Format; }
    // Waits until the D3D12 queue finished every submitted evaluation.
    void Idle() {
        if(!fence12 || fence12->GetCompletedValue()>=fenceValue) return;
        if(FAILED(fence12->SetEventOnCompletion(fenceValue,event)) || WaitForSingleObject(event,5000)!=WAIT_OBJECT_0)
            throw std::runtime_error("FSR D3D12 completion timeout");
    }
    void DestroyContext() {
        if(!context) return;
        Idle();
        auto result=ffx->destroy(&context,nullptr); context=nullptr;
        Log("FSR_CONTEXT_DESTROYED destroy=%s",FfxResultName(result));
    }
    void Prepare(const UpscalerFrame& frame) {
        if(frame.renderWidth!=renderWidth || frame.renderHeight!=renderHeight || frame.outputWidth!=outputWidth || frame.outputHeight!=outputHeight || !color.d3d11) {
            DestroyContext();
            color=Create(Format(frame.color),frame.renderWidth,frame.renderHeight,false);
            depth=Create(Format(frame.depth),frame.renderWidth,frame.renderHeight,false);
            motion=Create(Format(frame.motion),frame.renderWidth,frame.renderHeight,false);
            output=Create(Format(frame.output),frame.outputWidth,frame.outputHeight,true);
            renderWidth=frame.renderWidth; renderHeight=frame.renderHeight; outputWidth=frame.outputWidth; outputHeight=frame.outputHeight;
        }
        uint32_t flags=0;
        if(current.hdr) flags|=FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE|FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
        if(frame.camera->reversedDepth) flags|=FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
        if(frame.perceptual) flags|=FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
        if(context && flags==contextFlags) return;
        DestroyContext();
        ffxCreateContextDescUpscale create{}; create.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        create.flags=flags; create.maxRenderSize={renderWidth,renderHeight}; create.maxUpscaleSize={outputWidth,outputHeight}; create.fpMessage=&UpscaleMessage;
        ffxCreateContextDescUpscaleVersion version{}; version.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION; version.version=FFX_UPSCALER_VERSION;
        ffxCreateBackendDX12Desc backend{}; backend.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12; backend.device=device12.Get();
        create.header.pNext=&version.header; version.header.pNext=&backend.header;
        RequireFfx(ffx->create(&context,&create.header,nullptr),"FSR upscaling context");
        contextFlags=flags;
        ffxQueryGetProviderVersion provider{}; provider.header.type=FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        ffx->query(&context,&provider.header);
        Log("FSR_CONTEXT provider=%s available=[%s] render=%ux%u output=%ux%u flags=0x%x hdr=%d autoExposure=%d depthInverted=%d nonLinear=%d",
            provider.versionName?provider.versionName:"unknown",versions.c_str(),renderWidth,renderHeight,outputWidth,outputHeight,flags,
            current.hdr,current.hdr,frame.camera->reversedDepth,frame.perceptual);
    }
public:
    explicit FsrFeature(ID3D11Device* device) {
        ffx=&LoadFfxRuntime(L"amd_fidelityfx_upscaler_dx12.dll");
        RequireHr(device->QueryInterface(IID_PPV_ARGS(&device11)),"FSR D3D11.1 device");
        RequireHr(device->QueryInterface(IID_PPV_ARGS(&device5)),"FSR D3D11.4 device (shared fences)");
        ComPtr<IDXGIDevice> dxgi; RequireHr(device->QueryInterface(IID_PPV_ARGS(&dxgi)),"FSR DXGI device");
        ComPtr<IDXGIAdapter> adapter; RequireHr(dxgi->GetAdapter(&adapter),"FSR adapter");
        // D3D12 returns the adapter's existing device when the presentation bridge already made one.
        RequireHr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device12)),"FSR same-adapter D3D12 device");
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        RequireHr(device12->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"FSR D3D12 queue");
        RequireHr(device12->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence12)),"FSR shared fence");
        HANDLE handle{}; RequireHr(device12->CreateSharedHandle(fence12.Get(),nullptr,GENERIC_ALL,nullptr,&handle),"FSR shared fence handle");
        HRESULT opened=device5->OpenSharedFence(handle,IID_PPV_ARGS(&fence11)); CloseHandle(handle);
        RequireHr(opened,"FSR shared fence on D3D11");
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr); if(!event) throw std::runtime_error("FSR completion event");
        for(auto& r:recordings) {
            RequireHr(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&r.allocator)),"FSR allocator");
            RequireHr(device12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,r.allocator.Get(),nullptr,IID_PPV_ARGS(&r.list)),"FSR command list");
            RequireHr(r.list->Close(),"FSR command list close");
        }
        uint64_t count=8; uint64_t ids[8]{}; const char* names[8]{};
        ffxQueryDescGetVersions query{}; query.header.type=FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        query.createDescType=FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE; query.device=device12.Get();
        query.outputCount=&count; query.versionIds=ids; query.versionNames=names;
        if(ffx->query(nullptr,&query.header)==FFX_API_RETURN_OK)
            for(uint64_t i=0;i<std::min<uint64_t>(count,8);++i){ if(i) versions+=", "; versions+=names[i]?names[i]:"?"; if(!i && names[i]) defaultVersion=names[i]; }
        QueryPerformanceFrequency(&frequency);
        Log("FSR_READY D3D11 frame with private D3D12 queue on the same adapter; versions=[%s] (first is the default)",versions.c_str());
    }
    ~FsrFeature() override {
        try { DestroyContext(); } catch(...) {}
        if(event) CloseHandle(event);
    }
    UpscalerSizes Sizes(const UpscalerOptions& o) override {
        uint32_t width=0, height=0;
        ffxQueryDescUpscaleGetRenderResolutionFromQualityMode query{}; query.header.type=FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
        query.displayWidth=o.outputWidth; query.displayHeight=o.outputHeight; query.qualityMode=QualityMode(o.quality);
        query.pOutRenderWidth=&width; query.pOutRenderHeight=&height;
        RequireFfx(ffx->query(nullptr,&query.header),"FSR render resolution");
        // FSR accepts any render size up to the context's maximum.
        return {width,height,1,1,o.outputWidth,o.outputHeight};
    }
    void Configure(UpscalerOptions& o) override {
        if(o.alpha) {
            Log(ModLogLevel::Warning,"TEMPORAL_OUTLINES unavailable: FSR has no alpha upscaling; outlines drawn after reconstruction");
            o.alpha=false;
        }
        current=o;
    }
    void BeginFrame() override {}
    void Evaluate(ID3D11DeviceContext* immediate, const UpscalerFrame& frame) override {
        Prepare(frame);
        ComPtr<ID3D11DeviceContext4> context4; RequireHr(immediate->QueryInterface(IID_PPV_ARGS(&context4)),"FSR D3D11.4 context");
        immediate->CopyResource(color.d3d11.Get(),frame.color);
        immediate->CopyResource(depth.d3d11.Get(),frame.depth);
        immediate->CopyResource(motion.d3d11.Get(),frame.motion);
        RequireHr(context4->Signal(fence11.Get(),++fenceValue),"FSR D3D11 inputs signal");
        immediate->Flush();
        RequireHr(queue->Wait(fence12.Get(),fenceValue),"FSR D3D12 inputs wait");
        auto& recording=recordings[recordingIndex++%recordings.size()];
        if(fence12->GetCompletedValue()<recording.done) {
            RequireHr(fence12->SetEventOnCompletion(recording.done,event),"FSR recording completion");
            if(WaitForSingleObject(event,5000)!=WAIT_OBJECT_0) throw std::runtime_error("FSR recording completion timeout");
        }
        RequireHr(recording.allocator->Reset(),"FSR allocator reset");
        RequireHr(recording.list->Reset(recording.allocator.Get(),nullptr),"FSR command list reset");
        LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
        float delta=last.QuadPart?static_cast<float>(1000.0*(now.QuadPart-last.QuadPart)/frequency.QuadPart):16.667f; last=now;
        const auto& camera=*frame.camera;
        ffxDispatchDescUpscale dispatch{}; dispatch.header.type=FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        dispatch.commandList=recording.list.Get();
        dispatch.color=ffxApiGetResourceDX12(color.d3d12.Get(),FFX_API_RESOURCE_STATE_COMMON);
        dispatch.depth=ffxApiGetResourceDX12(depth.d3d12.Get(),FFX_API_RESOURCE_STATE_COMMON);
        dispatch.motionVectors=ffxApiGetResourceDX12(motion.d3d12.Get(),FFX_API_RESOURCE_STATE_COMMON);
        dispatch.output=ffxApiGetResourceDX12(output.d3d12.Get(),FFX_API_RESOURCE_STATE_COMMON,FFX_API_RESOURCE_USAGE_UAV);
        // Jitter is the projection shift in render pixels (y down), as FSR expects.
        dispatch.jitterOffset={camera.jitterX,camera.jitterY};
        // Current-to-previous UV motion; FSR expects render pixels.
        dispatch.motionVectorScale={camera.motionScaleX*renderWidth,camera.motionScaleY*renderHeight};
        dispatch.renderSize={renderWidth,renderHeight}; dispatch.upscaleSize={outputWidth,outputHeight};
        dispatch.enableSharpening=false; // RCAS runs separately after reconstruction.
        dispatch.frameTimeDelta=std::clamp(delta,.1f,1000.f); dispatch.preExposure=1.f; dispatch.reset=frame.reset;
        dispatch.cameraNear=camera.nearPlane; dispatch.cameraFar=camera.farPlane; dispatch.cameraFovAngleVertical=camera.fov;
        dispatch.viewSpaceToMetersFactor=1.f; // Unity units are meters.
        dispatch.flags=frame.perceptual?FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB:0;
        RequireFfx(ffx->dispatch(&context,&dispatch.header),"FSR upscale dispatch");
        RequireHr(recording.list->Close(),"FSR command list close");
        ID3D12CommandList* lists[]{recording.list.Get()}; queue->ExecuteCommandLists(1,lists);
        RequireHr(queue->Signal(fence12.Get(),++fenceValue),"FSR D3D12 output signal"); recording.done=fenceValue;
        RequireHr(context4->Wait(fence11.Get(),fenceValue),"FSR D3D11 output wait");
        immediate->CopyResource(frame.output,output.d3d11.Get());
    }
    std::string Release() override {
        try { DestroyContext(); return "fsrContext=destroyed"; }
        catch(const std::exception& e) { return std::string("fsrContext=")+e.what(); }
    }
    unsigned PresentedFrames() const override { return 0; }
    int Kind() const override { return 2; }
    // Version names start with the major version ("3.1.5", "4.0.2").
    bool DefaultIsFsr4() const { return !defaultVersion.empty() && defaultVersion[0]>='4' && defaultVersion[0]<='9'; }
};
std::unique_ptr<UpscalerFeature> CreateFsrFeature(ID3D11Device* device) { return std::make_unique<FsrFeature>(device); }
std::unique_ptr<UpscalerFeature> CreateFsr4Feature(ID3D11Device* device) {
    auto feature=std::make_unique<FsrFeature>(device);
    if(!feature->DefaultIsFsr4()) return nullptr;
    return feature;
}
