#pragma once
#include "Upscaling/UpscalerContext.h"
#include <cstdlib>
#include <memory>
#include <string>

// Super-resolution backend (DLSS, FSR3/4 on D3D12 or FSR 3 on D3D11). Each upscaler context owns one
// feature. Calls run on Unity's render thread with streamlineMutex held and
// the context's private D3D11 state bound; failures throw.
struct UpscalerOptions {
    int quality;              // UpscalerDescription quality; 0 renders at 100%
    UINT outputWidth, outputHeight;
    bool hdr;                 // linear HDR scene color (before post-processing)
    bool alpha;               // reconstruct the input alpha (temporal outlines)
    int preset;               // sl::DLSSPreset value for every mode; 0 = NVIDIA's per-mode default (DLSS only)
};
// Render sizes accepted for a quality. A zero min/max reports only the optimal size.
struct UpscalerSizes { UINT optimalWidth, optimalHeight, minWidth, minHeight, maxWidth, maxHeight; };
struct UpscalerFrame {
    int frame;
    bool reset;
    bool perceptual;          // color is sRGB-encoded (end-of-effects perceptual input)
    const CameraFrame* camera;
    // Render-sized RGBA16F color, R32F depth and RG16F current-to-previous UV motion.
    ID3D11Texture2D *color, *depth, *motion;
    ID3D11Texture2D* output; // Display-sized RGBA16F, unordered access
    UINT renderWidth, renderHeight, outputWidth, outputHeight;
};
class UpscalerFeature {
public:
    virtual ~UpscalerFeature() = default;
    virtual UpscalerSizes Sizes(const UpscalerOptions&) = 0;
    // Clears options.alpha when the backend cannot reconstruct alpha.
    virtual void Configure(UpscalerOptions&) = 0;
    // Once per frame that will be evaluated, before its inputs are validated.
    virtual void BeginFrame() = 0;
    virtual void Evaluate(ID3D11DeviceContext*, const UpscalerFrame&) = 0;
    // Releases the configured feature; never throws. Returns detail for the retirement log.
    virtual std::string Release() = 0;
    // Present calls serviced by the backend's presentation hooks (diagnostics).
    virtual unsigned PresentedFrames() const = 0;
    // Values match RiskOfRainUpscaling_UpscalerKind: 1 DLSS, 2 FSR3/4 DX12, 3 FSR 3 DX11.
    virtual int Kind() const = 0;
};
// Values match [Upscaling] Upscaler and RiskOfRainUpscaling_SetUpscalerBackend.
enum class UpscalerBackend { Auto, DLSS, FSR, FSR3DX11 }; // FSR is the FSR3/4 DX12 path.
// The preference captured by RiskOfRainUpscaling_UpscalerCreate for its context.
UpscalerBackend RequestedUpscaler();
// The DLSS preset captured by RiskOfRainUpscaling_UpscalerCreate for its context.
int RequestedDlssPreset();
// Null with a reason when no backend supports this device's adapter. Throws
// when the selected backend fails to start; Auto falls back from DLSS to FSR, and
// uses FSR3/4 DX12 only when the SDK offers FSR 4, otherwise FSR 3 DX11.
std::unique_ptr<UpscalerFeature> CreateUpscalerFeature(ID3D11Device*, UpscalerBackend, std::string& unavailable);
std::unique_ptr<UpscalerFeature> CreateDlssFeature(ID3D11Device*);
// True once DLSS started in this process; later DLSS failures stay latched until restart.
bool DlssEverReady();
std::unique_ptr<UpscalerFeature> CreateFsrFeature(ID3D11Device*);
// The major version in a FidelityFX version name: its first number, so "3.1.5", "4.0.2",
// "FSR 4.1.0" and "10.0.0" all read correctly; 0 without one. The SDK's version ids
// are opaque provider ids (never to be hard-coded), so the name is what says FSR 4.
inline unsigned long FfxMajorVersion(const std::string& name) {
    const auto digit=name.find_first_of("0123456789");
    return digit==std::string::npos?0:std::strtoul(name.c_str()+digit,nullptr,10);
}
// The FSR3/4 DX12 feature when the SDK's default upscaler for this adapter is FSR 4, otherwise null.
std::unique_ptr<UpscalerFeature> CreateFsr4Feature(ID3D11Device*);
std::unique_ptr<UpscalerFeature> CreateFsrDx11Feature(ID3D11Device*);
