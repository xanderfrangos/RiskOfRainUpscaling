#include "Upscaling/Upscaler.h"
#include "Presentation/StreamlinePresentation.h"
#include <atomic>
#include <cstdio>
#include <stdexcept>

static std::atomic<int> requestedUpscaler{static_cast<int>(UpscalerBackend::Auto)};
// 0 Auto, 1 DLSS, 2 FSR3/4 DX12, 3 FSR 3 DX11. Applies to contexts created afterwards.
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_SetUpscalerBackend(int backend) {
    if(backend<0 || backend>3) return -1;
    requestedUpscaler.store(backend); return 0;
}
UpscalerBackend RequestedUpscaler() { return static_cast<UpscalerBackend>(requestedUpscaler.load()); }
static std::atomic<int> requestedPreset{0};
// sl::DLSSPreset: 0 default per mode, 5 E (CNN), 10 J, 11 K, 12 L, 13 M. Applies to contexts created afterwards.
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_SetDlssPreset(int preset) {
    if(preset!=0 && preset!=5 && (preset<10 || preset>13)) return -1;
    requestedPreset.store(preset); return 0;
}
int RequestedDlssPreset() { return requestedPreset.load(); }

std::unique_ptr<UpscalerFeature> CreateUpscalerFeature(ID3D11Device* device, UpscalerBackend backend, std::string& unavailable) {
    ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter; ComPtr<IDXGIAdapter1> adapter1; DXGI_ADAPTER_DESC1 desc{};
    if(FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter.As(&adapter1)) || FAILED(adapter1->GetDesc1(&desc)))
        throw std::runtime_error("Upscaler adapter description");
    char reason[160]{};
    if(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
        sprintf_s(reason,"no super-resolution backend for software adapter vendor 0x%04x",desc.VendorId);
        unavailable=reason; return nullptr;
    }
    const bool nvidia=desc.VendorId==NvidiaVendorId;
    // Never load NVIDIA runtimes for another vendor's adapter.
    if(backend==UpscalerBackend::DLSS && !nvidia) {
        sprintf_s(reason,"DLSS requires an NVIDIA adapter (vendor 0x%04x); select Auto or FSR",desc.VendorId);
        unavailable=reason; return nullptr;
    }
    // DLSS must see every Present, which only the preloader's early hooks route to it.
    // Without them it would stop after 60 evaluations and stay latched until restart.
    const bool presentation=StreamlinePresentationObserved();
    if(backend==UpscalerBackend::DLSS && !presentation) {
        unavailable="DLSS requires the early graphics hooks (preloader installed, [Diagnostics] TraceGraphicsStartup = true); select Auto or FSR, or restart with them";
        return nullptr;
    }
    if(backend==UpscalerBackend::DLSS || (backend==UpscalerBackend::Auto && nvidia)) {
        if(backend==UpscalerBackend::DLSS) return CreateDlssFeature(device);
        // Unsupported DLSS falls back to FSR; a DLSS failure after it ran stays latched until restart.
        if(!presentation) Log(ModLogLevel::Warning,"DLSS_UNAVAILABLE early graphics hooks missed, so DLSS cannot present; Auto selects FSR");
        else {
            try { return CreateDlssFeature(device); }
            catch(const std::exception& e) {
                if(DlssEverReady()) throw;
                Log(ModLogLevel::Warning,"DLSS_UNAVAILABLE %s; Auto selects FSR",e.what());
            }
        }
    }
    if(backend==UpscalerBackend::FSR) return CreateFsrFeature(device);
    // FSR 3 DX11 binds compute UAV u8, which needs feature level 11_1 (Unity's device has it).
    const auto level=static_cast<unsigned>(device->GetFeatureLevel());
    const bool fsr3Dx11=level>=D3D_FEATURE_LEVEL_11_1;
    if(backend==UpscalerBackend::FSR3DX11) {
        if(!fsr3Dx11) {
            sprintf_s(reason,"FSR 3 DX11 needs Direct3D feature level 11_1 (device 0x%x); select Auto or FSR3/4 DX12",level);
            unavailable=reason; return nullptr;
        }
        return CreateFsrDx11Feature(device);
    }
    // Auto without DLSS: FSR 4 needs D3D12, so the D3D12 path runs only when the SDK
    // offers FSR 4 for this adapter. FSR 3 runs on D3D11, without the D3D12 round trip.
    if(!fsr3Dx11) {
        Log("UPSCALER_AUTO feature level 0x%x is below FSR 3 DX11's 11_1; Auto selects FSR3/4 DX12",level);
        return CreateFsrFeature(device);
    }
    // FSR 4 is AMD-only: other vendors never start the D3D12 path to ask.
    if(desc.VendorId!=AmdVendorId) {
        Log("UPSCALER_AUTO FSR 4 needs an AMD adapter (vendor 0x%04x); Auto selects FSR 3 DX11",desc.VendorId);
        return CreateFsrDx11Feature(device);
    }
    // The SDK's answer belongs to the adapter: a device on another adapter asks again.
    // Serialized by streamlineMutex, like every call here.
    static LUID probedAdapter{};
    static int fsr4=-1; // for probedAdapter: -1 not asked yet, 0 not offered, 1 offered
    if(probedAdapter.LowPart!=desc.AdapterLuid.LowPart || probedAdapter.HighPart!=desc.AdapterLuid.HighPart) { probedAdapter=desc.AdapterLuid; fsr4=-1; }
    if(fsr4!=0) {
        try {
            if(auto feature=CreateFsr4Feature(device)) { fsr4=1; return feature; }
            Log("UPSCALER_AUTO FSR 4 not offered for this adapter; Auto selects FSR 3 DX11");
        } catch(const std::exception& e) {
            if(fsr4==1) throw;
            Log(ModLogLevel::Warning,"UPSCALER_AUTO FSR3/4 DX12 unavailable (%s); Auto selects FSR 3 DX11",e.what());
        }
        fsr4=0;
    }
    return CreateFsrDx11Feature(device);
}
