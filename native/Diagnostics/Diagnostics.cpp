#include "Diagnostics/Diagnostics.h"
#include "Core/FullscreenPass.h"
#include "shaders/Fullscreen.h"
#include "shaders/DepthPreview.h"
#include "shaders/MotionPreview.h"
#include "shaders/FinalCopy.h"
#include <memory>
#include <mutex>
#include <cmath>

namespace {
constexpr int CaptureTextures = 7; // sceneColor, sceneBeforeHud, depth, motion, depthPreview, motionPreview, finalColor
struct DiagnosticsContext
{
    DiagnosticsDescription description{};
    std::atomic<int> state{0};
    FullscreenPass pass;
    ComPtr<ID3D11PixelShader> depthShader, motionShader, finalShader;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11ShaderResourceView> depth, motion;
    ComPtr<ID3D11RenderTargetView> depthPreview, motionPreview, finalView;
    // Readable copy of the bound HUD target; the final shader flips it into finalColor.
    ComPtr<ID3D11Texture2D> finalCopy;
    ComPtr<ID3D11ShaderResourceView> finalCopyView;
    ComPtr<ID3D11Texture2D> live[CaptureTextures];
    // Main-thread arm/cancel and render-thread freeze/poll share captureMutex.
    std::mutex captureMutex;
    int captureState = 0, requestedFrame = 0;
    ComPtr<ID3D11Texture2D> frozen[CaptureTextures];
    ComPtr<ID3D11Query> captureQuery;
    void ReleaseCapture() { for (auto& value : frozen) value.Reset(); captureQuery.Reset(); }
    void Reset()
    {
        { std::lock_guard<std::mutex> guard(captureMutex); captureState = -1; ReleaseCapture(); }
        for (auto& value : live) value.Reset();
        finalCopyView.Reset(); finalCopy.Reset(); finalView.Reset(); motionPreview.Reset(); depthPreview.Reset();
        motion.Reset(); depth.Reset(); constants.Reset(); finalShader.Reset(); motionShader.Reset(); depthShader.Reset(); pass.Reset();
    }
};
void Require(HRESULT result, const char* operation) { ::Require(result, operation, "DIAGNOSTICS_FAILED"); }
void View(DiagnosticsContext& context, void* pointer, const char* label, bool input, bool output, void** view)
{
    const auto& d = context.description;
    OwnedView(context.pass.device.Get(), pointer, label, input, output ? d.outputWidth : d.inputWidth, output ? d.outputHeight : d.inputHeight, view);
}
DXGI_FORMAT CopyFamily(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
void DrawPreviews(DiagnosticsContext& context)
{
    const auto& d = context.description;
    auto saved = context.pass.Scope();
    context.pass.Draw(context.depthShader.Get(), context.depth.Get(), nullptr, context.depthPreview.Get(), d.inputWidth, d.inputHeight, context.constants.Get());
    context.pass.Draw(context.motionShader.Get(), context.motion.Get(), nullptr, context.motionPreview.Get(), d.inputWidth, d.inputHeight, context.constants.Get());
}
void CopyFinal(DiagnosticsContext& context)
{
    ComPtr<ID3D11RenderTargetView> bound; context.pass.immediate->OMGetRenderTargets(1, &bound, nullptr);
    ComPtr<ID3D11Resource> resource; if (bound) bound->GetResource(&resource);
    ComPtr<ID3D11Texture2D> source; if (resource) resource.As(&source);
    if (!source) throw std::runtime_error("No HUD camera target bound for the final copy");
    D3D11_TEXTURE2D_DESC from{}, previous{}; source->GetDesc(&from);
    if (from.Width != static_cast<UINT>(context.description.outputWidth) || from.Height != static_cast<UINT>(context.description.outputHeight) ||
        from.SampleDesc.Count != 1 || from.ArraySize != 1 || from.MipLevels != 1 || CopyFamily(from.Format) == DXGI_FORMAT_UNKNOWN)
        throw std::runtime_error("HUD camera target format/size unsupported for the final copy (SDR single-sample required)");
    if (context.finalCopy) context.finalCopy->GetDesc(&previous);
    if (!context.finalCopy || previous.Format != CopyFamily(from.Format)) {
        context.finalCopyView.Reset(); context.finalCopy.Reset();
        from.Format = CopyFamily(from.Format); from.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        from.Usage = D3D11_USAGE_DEFAULT; from.CPUAccessFlags = 0; from.MiscFlags = 0;
        Require(context.pass.device->CreateTexture2D(&from, nullptr, &context.finalCopy), "Final readable copy");
        D3D11_SHADER_RESOURCE_VIEW_DESC view{}; view.Format = LinearViewFormat(from.Format);
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; view.Texture2D.MipLevels = 1;
        Require(context.pass.device->CreateShaderResourceView(context.finalCopy.Get(), &view, &context.finalCopyView), "Final UNORM view");
    }
    auto saved = context.pass.Scope();
    context.pass.immediate->CopyResource(context.finalCopy.Get(), source.Get());
    context.pass.Draw(context.finalShader.Get(), context.finalCopyView.Get(), nullptr, context.finalView.Get(),
        context.description.outputWidth, context.description.outputHeight);
}
// Freezes every capture source on the requested frame, after the final copy.
void Freeze(DiagnosticsContext& context, int unityFrame)
{
    std::lock_guard<std::mutex> guard(context.captureMutex);
    if (context.captureState != 1 || unityFrame < context.requestedFrame) return;
    if (unityFrame != context.requestedFrame) {
        Log(ModLogLevel::Error, "CAPTURE_FAILED requested Unity frame %d was not rendered (now %d)", context.requestedFrame, unityFrame);
        context.captureState = -1; return;
    }
    for (int i = 0; i < CaptureTextures; ++i) context.pass.immediate->CopyResource(context.frozen[i].Get(), context.live[i].Get());
    context.pass.immediate->End(context.captureQuery.Get());
    context.captureState = 2;
    Log("CAPTURE_SUBMITTED unityFrame=%d", unityFrame);
}
void Poll(DiagnosticsContext& context)
{
    std::lock_guard<std::mutex> guard(context.captureMutex);
    if (context.captureState != 2) return;
    auto result = context.pass.immediate->GetData(context.captureQuery.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (result == S_OK) { context.captureState = 3; Log("CAPTURE_COMPLETE unityFrame=%d", context.requestedFrame); }
    else if (FAILED(result)) { Log(ModLogLevel::Error, "CAPTURE_FAILED query HRESULT=0x%08lx", result); context.captureState = -1; }
}
void Fail(DiagnosticsContext& context, const char* operation, const std::exception& exception)
{
    Log(ModLogLevel::Error, "DIAGNOSTICS_FAILED %s %s; diagnostics stay off for this camera; upscaling is unaffected", operation, exception.what());
    context.state = -2;
    std::lock_guard<std::mutex> guard(context.captureMutex);
    if (context.captureState == 1 || context.captureState == 2) context.captureState = -1;
}
void __stdcall DiagnosticsEvent(int stage, void* pointer)
{
    auto context = static_cast<DiagnosticsContext*>(pointer);
    if (!context) return;
    if (stage == DiagnosticsRetire) { context->Reset(); context->state.store(-1, std::memory_order_release); return; }
    if (context->state.load() < 0) return;
    try {
        if (stage == DiagnosticsPreviews) DrawPreviews(*context);
        else if (stage == DiagnosticsCapturePoll) Poll(*context);
        else throw std::runtime_error("Unknown diagnostics stage");
        context->state.store(1, std::memory_order_release);
    }
    catch (const std::exception& exception) { Fail(*context, stage == DiagnosticsPreviews ? "previews" : "event", exception); }
}
void __stdcall FinalCopyEvent(int unityFrame, void* pointer)
{
    auto context = static_cast<DiagnosticsContext*>(pointer);
    if (!context || context->state.load() < 0) return;
    try { CopyFinal(*context); Freeze(*context, unityFrame); context->state.store(1, std::memory_order_release); }
    catch (const std::exception& exception) { Fail(*context, "final copy", exception); }
}
}

extern "C" void* __cdecl RiskOfRainUpscaling_DiagnosticsCreate(const DiagnosticsDescription* description)
{
    if (!description || description->size != sizeof(DiagnosticsDescription) || description->version != 1) return nullptr;
    const auto& d = *description;
    if (d.inputWidth < 1 || d.inputHeight < 1 || d.outputWidth < 1 || d.outputHeight < 1 ||
        !std::isfinite(d.nearPlane) || !std::isfinite(d.farPlane) || d.nearPlane <= 0 || d.farPlane <= d.nearPlane ||
        !std::isfinite(d.depthRange) || d.depthRange <= 0 || !std::isfinite(d.motionGain) || d.motionGain <= 0) return nullptr;
    void* sources[CaptureTextures]{d.sceneColor, d.sceneBeforeHud, d.depth, d.motion, d.depthPreview, d.motionPreview, d.finalColor};
    for (auto source : sources) if (!source) return nullptr;
    try
    {
        auto context = std::make_unique<DiagnosticsContext>(); context->description = d;
        ComPtr<ID3D11Device> device; static_cast<ID3D11Resource*>(d.depth)->GetDevice(&device);
        context->pass.Create(device.Get(), FullscreenShader, sizeof(FullscreenShader));
        Require(device->CreatePixelShader(DepthPreviewShader, sizeof(DepthPreviewShader), nullptr, &context->depthShader), "Depth preview shader");
        Require(device->CreatePixelShader(MotionPreviewShader, sizeof(MotionPreviewShader), nullptr, &context->motionShader), "Motion preview shader");
        Require(device->CreatePixelShader(FinalCopyShader, sizeof(FinalCopyShader), nullptr, &context->finalShader), "Final copy shader");
        View(*context, d.depth, "diagnosticsDepth", true, false, reinterpret_cast<void**>(context->depth.GetAddressOf()));
        View(*context, d.motion, "diagnosticsMotion", true, false, reinterpret_cast<void**>(context->motion.GetAddressOf()));
        View(*context, d.depthPreview, "depthPreview", false, false, reinterpret_cast<void**>(context->depthPreview.GetAddressOf()));
        View(*context, d.motionPreview, "motionPreview", false, false, reinterpret_cast<void**>(context->motionPreview.GetAddressOf()));
        View(*context, d.finalColor, "finalColor", false, true, reinterpret_cast<void**>(context->finalView.GetAddressOf()));
        for (int i = 0; i < CaptureTextures; ++i)
            Require(static_cast<ID3D11Resource*>(sources[i])->QueryInterface(IID_PPV_ARGS(&context->live[i])), "Capture source texture");
        struct Constants { float nearPlane, farPlane, depthRange, motionGain; unsigned reversed; float padding[3]; };
        Constants constants{d.nearPlane, d.farPlane, d.depthRange, d.motionGain, static_cast<unsigned>(d.reversedDepth), {0,0,0}};
        D3D11_BUFFER_DESC buffer{}; buffer.ByteWidth = sizeof(constants); buffer.Usage = D3D11_USAGE_IMMUTABLE; buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA initial{&constants,0,0};
        Require(device->CreateBuffer(&buffer, &initial, &context->constants), "Preview constants");
        Log("DIAGNOSTICS_CREATED context=%p input=%dx%d output=%dx%d", context.get(), d.inputWidth, d.inputHeight, d.outputWidth, d.outputHeight);
        return context.release();
    }
    catch (const std::exception& exception) { Log(ModLogLevel::Error, "DIAGNOSTICS_CREATE_FAILED %s", exception.what()); return nullptr; }
}
extern "C" void* __cdecl RiskOfRainUpscaling_DiagnosticsGetEvent() { return reinterpret_cast<void*>(&DiagnosticsEvent); }
extern "C" void* __cdecl RiskOfRainUpscaling_DiagnosticsGetFinalCopyEvent() { return reinterpret_cast<void*>(&FinalCopyEvent); }
extern "C" int __cdecl RiskOfRainUpscaling_DiagnosticsState(void* pointer)
{
    return pointer ? static_cast<DiagnosticsContext*>(pointer)->state.load(std::memory_order_acquire) : -2;
}
extern "C" void __cdecl RiskOfRainUpscaling_DiagnosticsDestroy(void* pointer)
{
    auto context = static_cast<DiagnosticsContext*>(pointer);
    if (context && context->state.load(std::memory_order_acquire) == -1) delete context;
}
extern "C" int __cdecl RiskOfRainUpscaling_CaptureArm(void* pointer, const CaptureDescription* request)
{
    if (!pointer || !request || request->size != sizeof(*request) || request->version != 1 || request->unityFrame <= 0) return -1;
    auto& context = *static_cast<DiagnosticsContext*>(pointer);
    std::lock_guard<std::mutex> guard(context.captureMutex);
    if (context.state.load() < 0 || context.captureState == 1 || context.captureState == 2) return -1;
    try {
        void* destinations[CaptureTextures]{request->sceneColor, request->sceneBeforeHud, request->depth, request->motion,
            request->depthPreview, request->motionPreview, request->finalColor};
        ComPtr<ID3D11Texture2D> frozen[CaptureTextures];
        for (int i = 0; i < CaptureTextures; ++i) {
            if (!destinations[i]) throw std::runtime_error("Capture destination null");
            Require(static_cast<ID3D11Resource*>(destinations[i])->QueryInterface(IID_PPV_ARGS(&frozen[i])), "Capture destination");
            ComPtr<ID3D11Device> owner; frozen[i]->GetDevice(&owner);
            D3D11_TEXTURE2D_DESC from{}, to{}; frozen[i]->GetDesc(&to); context.live[i]->GetDesc(&from);
            if (owner.Get() != context.pass.device.Get() || frozen[i].Get() == context.live[i].Get() ||
                to.Width != from.Width || to.Height != from.Height || to.Format != from.Format || to.SampleDesc.Count != 1 || to.ArraySize != 1 || to.MipLevels != 1)
                throw std::runtime_error("Capture destination device/format/shape/alias mismatch");
        }
        ComPtr<ID3D11Query> query; D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT,0};
        Require(context.pass.device->CreateQuery(&desc, &query), "Capture completion query");
        for (int i = 0; i < CaptureTextures; ++i) context.frozen[i] = frozen[i];
        context.captureQuery = query; context.requestedFrame = request->unityFrame; context.captureState = 1;
        Log("CAPTURE_ARMED requestedUnityFrame=%d", request->unityFrame);
        return 0;
    } catch (const std::exception& exception) { Log(ModLogLevel::Error, "CAPTURE_ARM_FAILED %s", exception.what()); context.captureState = -1; return -1; }
}
extern "C" int __cdecl RiskOfRainUpscaling_CaptureState(void* pointer)
{
    if (!pointer) return -1;
    auto& context = *static_cast<DiagnosticsContext*>(pointer);
    std::lock_guard<std::mutex> guard(context.captureMutex);
    return context.captureState;
}
extern "C" void __cdecl RiskOfRainUpscaling_CaptureCancel(void* pointer)
{
    if (!pointer) return;
    auto& context = *static_cast<DiagnosticsContext*>(pointer);
    std::lock_guard<std::mutex> guard(context.captureMutex);
    if (context.captureState != -1 || context.state.load() >= 0) context.captureState = 0;
    context.ReleaseCapture();
}
