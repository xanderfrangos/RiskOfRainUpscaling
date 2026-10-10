#include "Upscaling/Upscaler.h"
#include "Presentation/StreamlinePresentation.h"
#include <sl.h>
#include <sl_helpers.h>
#include <sl_security.h>
#include <tlhelp32.h>
#include <stdexcept>
#include <cstring>

bool VerifyNvidiaSignature(const wchar_t* path) { return sl::security::verifyEmbeddedSignature(path); }

static void RequireUpscaler(HRESULT hr, const char* label) {
    if (FAILED(hr)) { Log(ModLogLevel::Error, "DLSS_FAILED %s HRESULT=0x%08lx", label, hr); throw std::runtime_error(label); }
}
static void RequireSL(sl::Result result, const char* label) {
    if (result != sl::Result::eOk) {
        Log(ModLogLevel::Error, "DLSS_FAILED %s=%s (%d)", label, sl::getResultAsStr(result), static_cast<int>(result));
        throw std::runtime_error(label);
    }
}
static void CLog(sl::LogType type, const char* message) {
    Log(type == sl::LogType::eError ? ModLogLevel::Error : type == sl::LogType::eWarn ? ModLogLevel::Warning : ModLogLevel::Debug,
        "STREAMLINE_LOG level=%d %s", static_cast<int>(type), message ? message : "");
}
// This owned runtime stays resident, including when the user switches Off.
// Feature resources are released on the render thread; SDK work never runs in DllMain.
struct Runtime {
    HMODULE module{};
    bool initialized{}, failed{}, ready{};
    ComPtr<ID3D11Device> device;
    PFun_slInit* init{}; PFun_slSetD3DDevice* setDevice{};
    PFun_slIsFeatureSupported* supported{}; PFun_slGetFeatureFunction* function{};
    PFun_slGetNewFrameToken* token{}; PFun_slSetConstants* constants{};
    PFun_slSetTagForFrame* tag{}; PFun_slEvaluateFeature* evaluate{}; PFun_slFreeResources* free{};
    PFun_slDLSSSetOptions* options{}; PFun_slDLSSGetOptimalSettings* optimal{};
    PFun_slUpgradeInterface* upgrade{};
    template<class T> T Proc(const char* name) {
        auto value = reinterpret_cast<T>(GetProcAddress(module, name));
        if (!value) throw std::runtime_error(name);
        return value;
    }
    void Start(ID3D11Device* owner) {
        if (failed) throw std::runtime_error("Owned DLAA runtime failed earlier; restart required");
        if (!StreamlinePresentationHealthy()) throw std::runtime_error("DLAA presentation coverage failed earlier; restart required");
        if (initialized) {
            if (device.Get() != owner) throw std::runtime_error("D3D11 device changed; restart required");
            return;
        }
        try {
            // Streamline/NGX modules must come from our own runtime folder; any other copy is foreign.
            auto directory = g_runtimeFolder + L"\\D3D11";
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
            if (snapshot == INVALID_HANDLE_VALUE) throw std::runtime_error("Vendor module ownership audit unavailable");
            MODULEENTRY32W item{}; item.dwSize = sizeof(item); bool foreign = false;
            if (Module32FirstW(snapshot, &item)) do {
                if (!_wcsnicmp(item.szModule, L"sl.", 3) || !_wcsnicmp(item.szModule, L"nvngx", 5)) {
                    std::wstring path = item.szExePath;
                    auto prefix = g_runtimeFolder + L"\\";
                    if (_wcsnicmp(path.c_str(), prefix.c_str(), prefix.size())) {
                        Log(ModLogLevel::Warning, "DLSS_FOREIGN_RUNTIME path=%ls", path.c_str()); foreign = true;
                    }
                }
            } while (Module32NextW(snapshot, &item));
            CloseHandle(snapshot);
            if (foreign) throw std::runtime_error("Another component owns NVIDIA runtime modules");
            auto path = directory + L"\\sl.interposer.dll";
            if (!VerifyNvidiaSignature(path.c_str())) throw std::runtime_error("Interposer signature invalid");
            module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module) throw std::runtime_error("Cannot load owned D3D11 runtime");
            wchar_t actual[32768]{}; GetModuleFileNameW(module, actual, 32768);
            if (_wcsicmp(actual, path.c_str())) throw std::runtime_error("Interposer resolved to a foreign path");
            init=Proc<PFun_slInit*>("slInit"); setDevice=Proc<PFun_slSetD3DDevice*>("slSetD3DDevice");
            supported=Proc<PFun_slIsFeatureSupported*>("slIsFeatureSupported"); function=Proc<PFun_slGetFeatureFunction*>("slGetFeatureFunction");
            token=Proc<PFun_slGetNewFrameToken*>("slGetNewFrameToken"); constants=Proc<PFun_slSetConstants*>("slSetConstants");
            tag=Proc<PFun_slSetTagForFrame*>("slSetTagForFrame"); evaluate=Proc<PFun_slEvaluateFeature*>("slEvaluateFeature"); free=Proc<PFun_slFreeResources*>("slFreeResources");
            upgrade=Proc<PFun_slUpgradeInterface*>("slUpgradeInterface");
            const wchar_t* paths[]{directory.c_str()}; const sl::Feature features[]{sl::kFeatureDLSS};
            sl::Preferences prefs{}; prefs.pathsToPlugins=paths; prefs.numPathsToPlugins=1;
            prefs.featuresToLoad=features; prefs.numFeaturesToLoad=1; prefs.renderAPI=sl::RenderAPI::eD3D11;
            prefs.flags=sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
            prefs.engine=sl::EngineType::eUnity; prefs.engineVersion="2021.3.33";
            prefs.projectId="b1ac70ad-8304-406a-8b07-0a45519cfeae";
            prefs.pathToLogsAndData=g_logFolder.c_str(); prefs.logMessageCallback=&CLog;
            prefs.logLevel=sl::LogLevel::eDefault;
            RequireSL(init(prefs, sl::kSDKVersion), "slInit");
            // Retain SDK ownership even if binding/support checks subsequently fail.
            initialized=true; device=owner;
            RequireSL(setDevice(owner), "slSetD3DDevice");
            ComPtr<IDXGIDevice> dxgi; RequireUpscaler(owner->QueryInterface(IID_PPV_ARGS(&dxgi)), "DXGI device");
            ComPtr<IDXGIAdapter> adapter; RequireUpscaler(dxgi->GetAdapter(&adapter), "DLAA adapter");
            DXGI_ADAPTER_DESC desc{}; RequireUpscaler(adapter->GetDesc(&desc), "DLAA adapter description");
            sl::AdapterInfo info{}; info.deviceLUID=reinterpret_cast<uint8_t*>(&desc.AdapterLuid); info.deviceLUIDSizeInBytes=sizeof(LUID);
            RequireSL(supported(sl::kFeatureDLSS, info), "slIsFeatureSupported DLSS");
            void* entry{}; RequireSL(function(sl::kFeatureDLSS,"slDLSSSetOptions",entry),"DLSS options entrypoint"); options=reinterpret_cast<PFun_slDLSSSetOptions*>(entry);
            entry=nullptr; RequireSL(function(sl::kFeatureDLSS,"slDLSSGetOptimalSettings",entry),"DLSS settings entrypoint"); optimal=reinterpret_cast<PFun_slDLSSGetOptimalSettings*>(entry);
            if (!options || !optimal) throw std::runtime_error("DLSS entrypoint null");
            EnableStreamlinePresentation(owner,upgrade);
            ready=true;
            Log("DLSS_RUNTIME_READY owned D3D11 SDK; DLAA/DLSS super resolution; Frame Generation OFF; manual late device binding");
        } catch (...) { failed=true; throw; }
    }
};
static Runtime* runtime = new Runtime(); // Process lifetime, no render work during static destruction.

static sl::DLSSMode Mode(int quality) {
    switch(quality) {
    case 1: return sl::DLSSMode::eMaxQuality;
    case 2: return sl::DLSSMode::eBalanced;
    case 3: return sl::DLSSMode::eMaxPerformance;
    case 4: return sl::DLSSMode::eUltraPerformance;
    default: return sl::DLSSMode::eDLAA;
    }
}
// DLSS through the process-owned D3D11 Streamline instance. Presentation runs
// through Streamline's manual hooks, which must service every evaluation.
class DlssFeature final : public UpscalerFeature {
    sl::ViewportHandle viewport{1};
    UpscalerOptions current{};
    unsigned lastPresentCount=0, unpresentedEvaluations=0;
    static sl::DLSSOptions Options(const UpscalerOptions& o, bool enabled) {
        sl::DLSSOptions options{}; options.mode=enabled?Mode(o.quality):sl::DLSSMode::eOff;
        options.outputWidth=o.outputWidth; options.outputHeight=o.outputHeight;
        // One preset for every mode; eDefault keeps NVIDIA's per-mode choice (K for DLAA/Quality/Balanced).
        const auto preset=static_cast<sl::DLSSPreset>(o.preset);
        options.dlaaPreset=options.qualityPreset=options.balancedPreset=options.performancePreset=
            options.ultraPerformancePreset=options.ultraQualityPreset=preset;
        // At the end-of-effects seam tone mapping has already run. Unity's half-float
        // copy is linear SDR; the end-of-effects path sRGB-encodes it for this LDR
        // contract. Before post-processing the scene is linear HDR.
        options.colorBuffersHDR=o.hdr?sl::Boolean::eTrue:sl::Boolean::eFalse; options.useAutoExposure=sl::Boolean::eTrue;
        // Temporal outlines carry their weight in the input alpha.
        options.alphaUpscalingEnabled=enabled && o.alpha?sl::Boolean::eTrue:sl::Boolean::eFalse; return options;
    }
public:
    UpscalerSizes Sizes(const UpscalerOptions& o) override {
        sl::DLSSOptimalSettings settings{};
        RequireSL(runtime->optimal(Options(o,true),settings),"DLSS optimal settings");
        return {settings.optimalRenderWidth,settings.optimalRenderHeight,settings.renderWidthMin,settings.renderHeightMin,settings.renderWidthMax,settings.renderHeightMax};
    }
    void Configure(UpscalerOptions& o) override {
        auto accepted=runtime->options(viewport,Options(o,true));
        if(accepted!=sl::Result::eOk && o.alpha) {
            // Without alpha upscaling the outlines keep their non-temporal path.
            Log(ModLogLevel::Warning,"TEMPORAL_OUTLINES unavailable: DLSS alpha upscaling rejected (%s); outlines drawn after reconstruction",sl::getResultAsStr(accepted));
            o.alpha=false; accepted=runtime->options(viewport,Options(o,true));
        }
        RequireSL(accepted,"DLSS set options"); current=o;
        lastPresentCount=StreamlinePresentedFrames();
    }
    void BeginFrame() override {
        if(!StreamlinePresentationHealthy()) throw std::runtime_error("Streamline presentation integration failed");
        auto presented=StreamlinePresentedFrames();
        unpresentedEvaluations=presented==lastPresentCount?unpresentedEvaluations+1:1;
        lastPresentCount=presented;
        if(unpresentedEvaluations>60) {
            FailStreamlinePresentation();
            throw std::runtime_error("Streamline presentation hooks did not service 60 consecutive evaluations; check early bootstrap coverage; restart required");
        }
    }
    void Evaluate(ID3D11DeviceContext* context, const UpscalerFrame& frame) override {
        const auto& f=*frame.camera;
        sl::Constants constants{};
        std::memcpy(&constants.cameraViewToClip[0].x,f.projection,sizeof(f.projection));
        std::memcpy(&constants.clipToCameraView[0].x,f.inverseProjection,sizeof(f.inverseProjection));
        std::memcpy(&constants.clipToPrevClip[0].x,f.clipToPrevious,sizeof(f.clipToPrevious));
        std::memcpy(&constants.prevClipToClip[0].x,f.previousToClip,sizeof(f.previousToClip));
        constants.jitterOffset={f.jitterX,f.jitterY}; constants.mvecScale={f.motionScaleX,f.motionScaleY}; constants.cameraPinholeOffset={0,0};
        constants.cameraPos={f.position[0],f.position[1],f.position[2]}; constants.cameraUp={f.up[0],f.up[1],f.up[2]};
        constants.cameraRight={f.right[0],f.right[1],f.right[2]}; constants.cameraFwd={f.forward[0],f.forward[1],f.forward[2]};
        constants.cameraNear=f.nearPlane; constants.cameraFar=f.farPlane; constants.cameraFOV=f.fov; constants.cameraAspectRatio=f.aspect;
        constants.depthInverted=f.reversedDepth?sl::Boolean::eTrue:sl::Boolean::eFalse;
        constants.cameraMotionIncluded=sl::Boolean::eTrue; constants.motionVectors3D=sl::Boolean::eFalse;
        constants.reset=frame.reset?sl::Boolean::eTrue:sl::Boolean::eFalse;
        constants.motionVectorsJittered=sl::Boolean::eFalse; constants.motionVectorsDilated=sl::Boolean::eFalse;
        sl::FrameToken* token=nullptr; uint32_t index=static_cast<uint32_t>(frame.frame);
        RequireSL(runtime->token(token,&index),"DLAA frame token"); if(!token) throw std::runtime_error("DLAA frame token null");
        RequireSL(runtime->constants(constants,*token,viewport),"DLAA constants");
        sl::Resource colors{sl::ResourceType::eTex2d,frame.color}, output{sl::ResourceType::eTex2d,frame.output};
        sl::Resource depth{sl::ResourceType::eTex2d,frame.depth}, motion{sl::ResourceType::eTex2d,frame.motion};
        sl::Extent extent{}; extent.width=frame.renderWidth; extent.height=frame.renderHeight;
        sl::Extent display{}; display.width=frame.outputWidth; display.height=frame.outputHeight;
        sl::ResourceTag tags[]{ {&colors,sl::kBufferTypeScalingInputColor,sl::ResourceLifecycle::eOnlyValidNow,&extent},
            {&output,sl::kBufferTypeScalingOutputColor,sl::ResourceLifecycle::eOnlyValidNow,&display},
            {&depth,sl::kBufferTypeDepth,sl::ResourceLifecycle::eOnlyValidNow,&extent},
            {&motion,sl::kBufferTypeMotionVectors,sl::ResourceLifecycle::eOnlyValidNow,&extent} };
        RequireSL(runtime->tag(*token,viewport,tags,4,context),"DLAA tags");
        const sl::BaseStructure* inputs[]{&viewport};
        RequireSL(runtime->evaluate(sl::kFeatureDLSS,*token,inputs,1,context),"slEvaluateFeature DLAA");
    }
    std::string Release() override {
        auto a=runtime->options(viewport,Options(current,false)); auto b=runtime->free(sl::kFeatureDLSS,viewport);
        return std::string("optionsOff=")+sl::getResultAsStr(a)+" free="+sl::getResultAsStr(b);
    }
    unsigned PresentedFrames() const override { return StreamlinePresentedFrames(); }
    int Kind() const override { return 1; }
};

bool DlssEverReady() { return runtime->ready; }
std::unique_ptr<UpscalerFeature> CreateDlssFeature(ID3D11Device* device) {
    runtime->Start(device);
    return std::make_unique<DlssFeature>();
}
