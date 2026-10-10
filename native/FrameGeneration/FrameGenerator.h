#pragma once
#include "Core/CameraFrame.h"
#include "Core/Native.h"
#include <memory>

// One Unity frame's Frame Generation inputs on the presenting D3D12 device.
// Depth/motion are render-sized; HUD-less color and UI alpha are display-sized.
struct FrameGenerationRequest {
    const CameraFrame& packet;
    ID3D12Resource *depth, *motion, *hudless;
    ID3D12Resource* uiAlpha;   // Null: ordinary HUD-less generation. Cleared when the backend rejects it.
    bool uiRejected;           // Set when the backend rejected UI recomposition
    const DXGI_SWAP_CHAIN_DESC1& desc;
    UINT interval, renderWidth, renderHeight;
    int multiplier;            // Requested total frames per rendered frame (2–6)
};
// The runtime's report after the display Present.
struct FrameGenerationPresented {
    unsigned frames = 0;                 // Frames the runtime presented for this Present
    ID3D12Fence* inputsFence = nullptr;  // Inputs may be reused once this fence reaches inputsValue
    UINT64 inputsValue = 0;
    unsigned status = 0;                 // Backend status bits, for logs
    const char* failure = nullptr;       // Set when active generation must latch off
};
// A Frame Generation runtime for the D3D12 presentation bridge. The facade in
// FrameGeneration.cpp owns requested/enabled state and failure latching; backends
// throw std::runtime_error from SDK failures.
class FrameGenerator {
public:
    virtual ~FrameGenerator() = default;
    // Loads the runtime before the presenter device is created.
    virtual void Start(IDXGIAdapter*, int multiplier) = 0;
    virtual void Bind(ID3D12Device*) = 0;
    // Creates the presenting queue and display swap chain for the bound device.
    virtual void CreateDisplay(ID3D12Device*, IDXGIFactory2*, HWND, const DXGI_SWAP_CHAIN_DESC1&,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, ComPtr<ID3D12CommandQueue>&, ComPtr<IDXGISwapChain1>&) = 0;
    // After the bridge drained its GPU work, before it releases the display swap chain.
    virtual void ReleaseDisplay() {}
    // Unity timing markers: 0 simulation start (paces the frame), 1 simulation end, 2 render submit start, 3 render submit end.
    virtual void Marker(int marker, int frame) = 0;
    virtual void PresentMarker(bool start, int frame) = 0;
    // The multiplier now generating, or 0 to skip this frame. offReason, when set,
    // asks the facade to suspend generation.
    virtual int Prepare(FrameGenerationRequest&, const char*& offReason) = 0;
    // True when generation was on and is now off. On failure returns false and sets error.
    virtual bool Off(const char*& error) = 0;
    virtual void ClearTags(int frame, unsigned width, unsigned height) = 0;
    // True when the runtime composites a separate UI layer: the bridge then presents
    // the HUD-less image and passes the HUD as premultiplied color to PresentUi.
    virtual bool LayersUi() const { return false; }
    // Before every display Present: this frame's UI layer, or null to present the final image.
    virtual void PresentUi(ID3D12Resource*) {}
    // Null on success, otherwise why the post-Present state query failed.
    virtual const char* AfterPresent(FrameGenerationPresented&) = 0;
    virtual unsigned MaxGenerated() const = 0;
    // Frame-start limiter: -1 when unavailable.
    virtual int FrameLimit() const = 0;
    virtual int SetFrameLimit(int microseconds) = 0;
    virtual int GuardWaits() const = 0;
};
// Frame Generation backends; values match [Frame Generation] Backend and RiskOfRainUpscaling_FrameGenerationKind.
enum class FrameGenerationBackend { Auto, DLSS, FSR };
std::unique_ptr<FrameGenerator> CreateDlssFrameGenerator();
std::unique_ptr<FrameGenerator> CreateFsrFrameGenerator();
