#include "Upscaling/Upscaler.h"
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/dx11/ffx_dx11.h>
#include <algorithm>
#include <stdexcept>
#include <vector>

static void RequireFfx(FfxErrorCode code, const char* label) {
    if (code != FFX_OK) { Log(ModLogLevel::Error, "FSR_DX11_FAILED %s=0x%08x", label, static_cast<unsigned>(code)); throw std::runtime_error(label); }
}
static void UpscaleMessage(FfxMsgType type, const wchar_t* message) {
    Log(type==FFX_MESSAGE_TYPE_ERROR?ModLogLevel::Error:ModLogLevel::Warning, "FSR_DX11_MESSAGE type=%d %ls", static_cast<int>(type), message?message:L"");
}
static FfxFsr3UpscalerQualityMode QualityMode(int quality) {
    switch(quality) {
    case 1: return FFX_FSR3UPSCALER_QUALITY_MODE_QUALITY;
    case 2: return FFX_FSR3UPSCALER_QUALITY_MODE_BALANCED;
    case 3: return FFX_FSR3UPSCALER_QUALITY_MODE_PERFORMANCE;
    case 4: return FFX_FSR3UPSCALER_QUALITY_MODE_ULTRA_PERFORMANCE;
    default: return FFX_FSR3UPSCALER_QUALITY_MODE_NATIVEAA;
    }
}
static FfxResource Resource(ID3D11Texture2D* texture, FfxResourceStates state) {
    FfxResource resource{};
    resource.resource=texture; resource.description=GetFfxResourceDescriptionDX11(texture); resource.state=state;
    return resource;
}
// The backend's own format mapping (ffx_dx11.cpp), defined without a header declaration.
DXGI_FORMAT ffxGetDX11FormatFromSurfaceFormat_Fsr31(FfxSurfaceFormat);

// AMD FSR 3.1 upscaling on Unity's own D3D11 device, through OptiScaler's
// FidelityFX-SDK-DX11 port: the passes dispatch on the immediate context inside
// the context's private state bracket, with no D3D12 device, copies or fences.
class FsrDx11Feature final : public UpscalerFeature {
    ComPtr<ID3D11Device> device;
    std::vector<unsigned char> scratch; // Backend state; zeroed, as ffxGetInterfaceDX11 requires.
    FfxInterface backend{};
    FfxFsr3UpscalerContext context{};
    bool created=false;
    uint32_t contextFlags=0;
    UINT renderWidth=0, renderHeight=0, outputWidth=0, outputHeight=0;
    UpscalerOptions current{};
    LARGE_INTEGER frequency{}, last{};
    // FSR 3.1's dilated depth, dilated motion and reconstructed previous depth are
    // the application's, sized by the context and passed on every dispatch. Left
    // null, the passes write and read nothing: no motion, so history smears.
    ComPtr<ID3D11Texture2D> dilatedDepth, dilatedMotion, previousNearestDepth;
    // Exposure 1 for LDR input, where FSR applies no auto exposure: what the port's
    // internal default (0, read as 1) and the DX12 runtime mean. Passed explicitly
    // because one build gave black LDR output on the internal default; bindings and
    // contents checked out and it has not reproduced since.
    ComPtr<ID3D11Texture2D> unitExposure;

    ComPtr<ID3D11Texture2D> Shared(const FfxCreateResourceDescription& description) {
        D3D11_TEXTURE2D_DESC d{}; d.Width=description.resourceDescription.width; d.Height=description.resourceDescription.height;
        d.MipLevels=d.ArraySize=1; d.Format=ffxGetDX11FormatFromSurfaceFormat_Fsr31(description.resourceDescription.format); d.SampleDesc.Count=1;
        d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Texture2D> texture;
        if(FAILED(device->CreateTexture2D(&d,nullptr,&texture))) { Log(ModLogLevel::Error,"FSR_DX11_FAILED shared resource %ux%u format=%d",d.Width,d.Height,d.Format); throw std::runtime_error("FSR DX11 shared resource"); }
        return texture;
    }
    void DestroyContext() {
        if(!created) return;
        created=false;
        auto result=ffxFsr3UpscalerContextDestroy(&context);
        Log("FSR_DX11_CONTEXT_DESTROYED destroy=0x%08x",static_cast<unsigned>(result));
    }
    void Prepare(const UpscalerFrame& frame) {
        uint32_t flags=0;
        if(current.hdr) flags|=FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE|FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
        if(frame.camera->reversedDepth) flags|=FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED;
        if(created && flags==contextFlags && frame.renderWidth==renderWidth && frame.renderHeight==renderHeight &&
           frame.outputWidth==outputWidth && frame.outputHeight==outputHeight) return;
        DestroyContext();
        renderWidth=frame.renderWidth; renderHeight=frame.renderHeight; outputWidth=frame.outputWidth; outputHeight=frame.outputHeight;
        FfxFsr3UpscalerContextDescription create{};
        create.flags=flags; create.maxRenderSize={renderWidth,renderHeight}; create.maxUpscaleSize={outputWidth,outputHeight};
        create.fpMessage=&UpscaleMessage; create.backendInterface=backend;
        RequireFfx(ffxFsr3UpscalerContextCreate(&context,&create),"FSR DX11 upscaling context");
        created=true;
        // A context without its shared resources would dispatch with none (no motion):
        // it is destroyed, so the next frame creates both again.
        try {
            FfxFsr3UpscalerSharedResourceDescriptions shared{};
            RequireFfx(ffxFsr3UpscalerGetSharedResourceDescriptions(&context,&shared),"FSR DX11 shared resource descriptions");
            dilatedDepth=Shared(shared.dilatedDepth); dilatedMotion=Shared(shared.dilatedMotionVectors); previousNearestDepth=Shared(shared.reconstructedPrevNearestDepth);
        } catch(...) { DestroyContext(); throw; }
        contextFlags=flags;
        Log("FSR_DX11_CONTEXT version=%d.%d.%d render=%ux%u output=%ux%u flags=0x%x hdr=%d autoExposure=%d depthInverted=%d",
            FFX_FSR3UPSCALER_VERSION_MAJOR,FFX_FSR3UPSCALER_VERSION_MINOR,FFX_FSR3UPSCALER_VERSION_PATCH,renderWidth,renderHeight,outputWidth,outputHeight,
            flags,current.hdr,current.hdr,frame.camera->reversedDepth);
    }
public:
    explicit FsrDx11Feature(ID3D11Device* value) : device(value) {
        // The luma pyramid pass writes u8; feature level 11_0 has only eight compute UAV slots.
        if(device->GetFeatureLevel()<D3D_FEATURE_LEVEL_11_1) throw std::runtime_error("FSR 3 DX11 needs Direct3D feature level 11_1");
        scratch.resize(ffxGetScratchMemorySizeDX11(FFX_FSR3UPSCALER_CONTEXT_COUNT));
        RequireFfx(ffxGetInterfaceDX11(&backend,ffxGetDeviceDX11_Fsr31(device.Get()),scratch.data(),scratch.size(),FFX_FSR3UPSCALER_CONTEXT_COUNT),"FSR DX11 backend interface");
        const float exposure[2]{1.f,1.f};
        D3D11_TEXTURE2D_DESC d{}; d.Width=d.Height=d.MipLevels=d.ArraySize=1; d.Format=DXGI_FORMAT_R32G32_FLOAT; d.SampleDesc.Count=1;
        d.Usage=D3D11_USAGE_IMMUTABLE; d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{exposure,sizeof(exposure),0};
        if(FAILED(device->CreateTexture2D(&d,&data,&unitExposure))) throw std::runtime_error("FSR DX11 exposure texture");
        QueryPerformanceFrequency(&frequency);
        Log("FSR_DX11_READY FSR %d.%d.%d on Unity's D3D11 device (FidelityFX-SDK-DX11); featureLevel=0x%x",
            FFX_FSR3UPSCALER_VERSION_MAJOR,FFX_FSR3UPSCALER_VERSION_MINOR,FFX_FSR3UPSCALER_VERSION_PATCH,static_cast<unsigned>(device->GetFeatureLevel()));
    }
    ~FsrDx11Feature() override { DestroyContext(); }
    UpscalerSizes Sizes(const UpscalerOptions& o) override {
        uint32_t width=0, height=0;
        RequireFfx(ffxFsr3UpscalerGetRenderResolutionFromQualityMode(&width,&height,o.outputWidth,o.outputHeight,QualityMode(o.quality)),"FSR DX11 render resolution");
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
        LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
        float delta=last.QuadPart?static_cast<float>(1000.0*(now.QuadPart-last.QuadPart)/frequency.QuadPart):16.667f; last=now;
        const auto& camera=*frame.camera;
        FfxFsr3UpscalerDispatchDescription dispatch{};
        dispatch.commandList=ffxGetCommandListDX11(immediate);
        dispatch.color=Resource(frame.color,FFX_RESOURCE_STATE_COMPUTE_READ);
        dispatch.depth=Resource(frame.depth,FFX_RESOURCE_STATE_COMPUTE_READ);
        dispatch.motionVectors=Resource(frame.motion,FFX_RESOURCE_STATE_COMPUTE_READ);
        dispatch.output=Resource(frame.output,FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch.dilatedDepth=Resource(dilatedDepth.Get(),FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch.dilatedMotionVectors=Resource(dilatedMotion.Get(),FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch.reconstructedPrevNearestDepth=Resource(previousNearestDepth.Get(),FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        if(!current.hdr) dispatch.exposure=Resource(unitExposure.Get(),FFX_RESOURCE_STATE_COMPUTE_READ);
        // Jitter is the projection shift in render pixels (y down), as FSR expects.
        dispatch.jitterOffset={camera.jitterX,camera.jitterY};
        // Current-to-previous UV motion; FSR expects render pixels.
        dispatch.motionVectorScale={camera.motionScaleX*renderWidth,camera.motionScaleY*renderHeight};
        dispatch.renderSize={renderWidth,renderHeight}; dispatch.upscaleSize={outputWidth,outputHeight};
        dispatch.enableSharpening=false; // RCAS runs separately after reconstruction.
        dispatch.frameTimeDelta=std::clamp(delta,.1f,1000.f); dispatch.preExposure=1.f; dispatch.reset=frame.reset;
        dispatch.cameraNear=camera.nearPlane; dispatch.cameraFar=camera.farPlane; dispatch.cameraFovAngleVertical=camera.fov;
        dispatch.viewSpaceToMetersFactor=1.f; // Unity units are meters.
        RequireFfx(ffxFsr3UpscalerContextDispatch(&context,&dispatch),"FSR DX11 upscale dispatch");
    }
    std::string Release() override {
        DestroyContext();
        return "fsrDx11Context=destroyed";
    }
    unsigned PresentedFrames() const override { return 0; }
    int Kind() const override { return 3; }
    // FSR expects linear colour and 3.1.2 cannot be told otherwise (the non-linear flags came in 3.1.4).
    bool PerceptualLdr() const override { return false; }
};
std::unique_ptr<UpscalerFeature> CreateFsrDx11Feature(ID3D11Device* device) { return std::make_unique<FsrDx11Feature>(device); }
