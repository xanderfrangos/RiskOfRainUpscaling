#include "Upscaling/Upscaler.h"
#include <atomic>
#include <cstdio>
#include <stdexcept>

static std::atomic<int> requestedUpscaler{static_cast<int>(UpscalerBackend::Auto)};
// 0 Auto, 1 DLSS, 2 FSR. Applies to contexts created afterwards.
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_SetUpscalerBackend(int backend) {
    if(backend<0 || backend>2) return -1;
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
    if(backend==UpscalerBackend::DLSS || (backend==UpscalerBackend::Auto && nvidia)) {
        if(backend==UpscalerBackend::DLSS) return CreateDlssFeature(device);
        // Unsupported DLSS falls back to FSR; a DLSS failure after it ran stays latched until restart.
        try { return CreateDlssFeature(device); }
        catch(const std::exception& e) {
            if(DlssEverReady()) throw;
            Log(ModLogLevel::Warning,"DLSS_UNAVAILABLE %s; Auto selects FSR",e.what());
        }
    }
    return CreateFsrFeature(device);
}
