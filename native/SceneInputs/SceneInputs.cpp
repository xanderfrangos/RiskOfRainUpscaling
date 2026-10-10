#include "SceneInputs/SceneInputs.h"
#include "Upscaling/UpscalerContext.h"
#include "Core/FullscreenPass.h"
#include "Diagnostics/Profiler.h"
#include "shaders/Fullscreen.h"
#include "shaders/RawDepth.h"
#include <memory>
#include <cmath>

struct SceneInputsContext
{
    SceneInputsDescription description{};
    std::atomic<int> state{0}; // 0 ready, 1 rendered, -2 failed, -1 retired
    std::atomic<unsigned> frame{0};
    // depth, motion, G-buffer and G-buffer depth width/height of the latest frame.
    std::atomic<int> dimensions[8];
    bool geometryReady = false; // render-thread only; consumed exactly once
    std::atomic<int> stageMask{0};
    int unityFrame = 0;
    DWORD renderThread = 0;
    // Scene color bound after transparents (stage 100); pre-post DLAA reads and
    // writes it in place. Render-thread only; cleared by the next geometry probe.
    ComPtr<ID3D11Texture2D> sceneColor;
    int sceneColorFrame = 0;
    // Scene depth bound at the G-buffer probe. Depth is captured once, after
    // transparents; this same-frame buffer is the source if the DSV is unbound then.
    ComPtr<ID3D11Resource> geometryDepth;
    SceneInputsContext() { for (auto& dimension : dimensions) dimension.store(0); }
    FullscreenPass pass;
    ComPtr<ID3D11PixelShader> rawDepthShader;
    ComPtr<ID3D11Texture2D> depthCopy;
    ComPtr<ID3D11ShaderResourceView> depthCopyView;
    ComPtr<ID3D11RenderTargetView> rawDepthView;
    void Reset()
    {
        sceneColor.Reset(); sceneColorFrame = 0; geometryDepth.Reset();
        rawDepthShader.Reset(); depthCopy.Reset(); depthCopyView.Reset(); rawDepthView.Reset(); pass.Reset();
    }
};
static void Require(HRESULT result, const char* operation) { Require(result, operation, "SCENE_INPUTS_FAILED"); }
extern "C" void* __cdecl RiskOfRainUpscaling_SceneInputsCreate(const SceneInputsDescription* description)
{
    if (!description || description->size != sizeof(SceneInputsDescription) || description->version != 4 ||
        description->width < 1 || description->height < 1 || !description->depth) return nullptr;
    try
    {
        auto context = std::make_unique<SceneInputsContext>(); context->description = *description;
        ComPtr<ID3D11Device> device; static_cast<ID3D11Resource*>(description->depth)->GetDevice(&device);
        context->pass.Create(device.Get(), FullscreenShader, sizeof(FullscreenShader));
        Require(device->CreatePixelShader(RawDepthShader, sizeof(RawDepthShader), nullptr, &context->rawDepthShader), "Scene inputs raw depth shader");
        OwnedView(device.Get(), description->depth, "rawDepthOutput", false, description->width, description->height, reinterpret_cast<void**>(context->rawDepthView.GetAddressOf()));
        Log("SCENE_INPUTS_CREATED created=%p device=%p size=%dx%d ABI=%u", context.get(), device.Get(), description->width, description->height, description->size);
        return context.release();
    }
    catch (const std::exception& exception) { Log(ModLogLevel::Error, "SCENE_INPUTS_CREATE_FAILED %s", exception.what()); return nullptr; }
}

static void Describe(ID3D11Resource* resource, const char* label, unsigned frame, int stage, bool log, std::atomic<int>* dimensions=nullptr)
{
    if (dimensions) { dimensions[0] = 0; dimensions[1] = 0; }
    if (!resource) { if (log) Log(ModLogLevel::Debug, "SCENE_INPUTS_SOURCE frame=%u stage=%d %s=null", frame, stage, label); return; }
    ComPtr<ID3D11Texture2D> texture;
    if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture))))
    {
        D3D11_TEXTURE2D_DESC description{}; texture->GetDesc(&description);
        if (dimensions) { dimensions[0] = static_cast<int>(description.Width); dimensions[1] = static_cast<int>(description.Height); }
        if (log) Log(ModLogLevel::Debug, "SCENE_INPUTS_SOURCE frame=%u stage=%d %s=%p size=%ux%u format=%u samples=%u", frame, stage, label, texture.Get(),
            description.Width, description.Height, description.Format, description.SampleDesc.Count);
    }
}
static bool Sampled(unsigned frame) { return frame <= 4 || frame % 300 == 0; }
static void CaptureDepth(SceneInputsContext& context, int stage)
{
    auto immediate = context.pass.immediate.Get();
    ComPtr<ID3D11DepthStencilView> bound;
    immediate->OMGetRenderTargets(0, nullptr, &bound);
    ComPtr<ID3D11Resource> resource;
    if (bound) bound->GetResource(&resource);
    // AfterForwardAlpha may have released depth. The buffer bound at this frame's
    // G-buffer probe still holds its depth; never reuse old frames.
    else resource = context.geometryDepth;
    context.geometryDepth.Reset();
    if (!resource) throw std::runtime_error("No same-frame scene depth available");
    Describe(resource.Get(), "sceneDepthDSV", context.frame, stage, Sampled(context.frame), context.dimensions);
    ComPtr<ID3D11Texture2D> source; Require(resource.As(&source), "Depth texture interface");
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    if (desc.Width != static_cast<UINT>(context.description.width) || desc.Height != static_cast<UINT>(context.description.height) ||
        desc.SampleDesc.Count != 1 || desc.ArraySize != 1 || desc.MipLevels != 1)
        throw std::runtime_error("Scene depth dimensions/MSAA unsupported");
    DXGI_FORMAT viewFormat;
    switch (desc.Format)
    {
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT:
        desc.Format = DXGI_FORMAT_R24G8_TYPELESS; viewFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS; viewFormat = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT:
        desc.Format = DXGI_FORMAT_R32_TYPELESS; viewFormat = DXGI_FORMAT_R32_FLOAT; break;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM:
        desc.Format = DXGI_FORMAT_R16_TYPELESS; viewFormat = DXGI_FORMAT_R16_UNORM; break;
    default: throw std::runtime_error("Unsupported scene depth format");
    }
    D3D11_TEXTURE2D_DESC previous{}; if (context.depthCopy) context.depthCopy->GetDesc(&previous);
    if (!context.depthCopy || previous.Format != desc.Format)
    {
        context.depthCopyView.Reset(); context.depthCopy.Reset();
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE; desc.Usage = D3D11_USAGE_DEFAULT; desc.CPUAccessFlags = 0; desc.MiscFlags = 0;
        Require(context.pass.device->CreateTexture2D(&desc, nullptr, &context.depthCopy), "Depth copy allocation");
        D3D11_SHADER_RESOURCE_VIEW_DESC view{}; view.Format = viewFormat; view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; view.Texture2D.MipLevels = 1;
        Require(context.pass.device->CreateShaderResourceView(context.depthCopy.Get(), &view, &context.depthCopyView), "Depth copy view");
        Log("SCENE_INPUTS_DEPTH_COPY source=%p nativeCopy=%p format=%u viewFormat=%u bind=0x%x", source.Get(), context.depthCopy.Get(), desc.Format, viewFormat, desc.BindFlags);
    }
    auto saved = context.pass.Scope();
    ProfileScope profile(PROFILE_LABEL("inputs.depthCapture (copy + raw depth draw)"));
    immediate->CopyResource(context.depthCopy.Get(), source.Get());
    context.pass.Draw(context.rawDepthShader.Get(), context.depthCopyView.Get(), nullptr, context.rawDepthView.Get(), context.description.width, context.description.height);
    context.stageMask.fetch_or(2);
}
static void ProbeGeometry(SceneInputsContext& context)
{
    context.renderThread = GetCurrentThreadId();
    context.geometryReady = false;
    context.sceneColor.Reset(); context.sceneColorFrame = 0;
    ++context.frame; context.stageMask = 0;
    for (auto& dimension : context.dimensions) dimension = 0;
    ComPtr<ID3D11RenderTargetView> targets[8]; ID3D11RenderTargetView* pointers[8]{};
    ComPtr<ID3D11DepthStencilView> depth;
    context.pass.immediate->OMGetRenderTargets(8, pointers, &depth);
    bool found = false;
    for (int i = 0; i < 8; ++i)
    {
        targets[i].Attach(pointers[i]); if (!targets[i]) continue;
        ComPtr<ID3D11Resource> resource; targets[i]->GetResource(&resource);
        Describe(resource.Get(), "GBufferTarget", context.frame, 90, Sampled(context.frame), found ? nullptr : context.dimensions + 4);
        ComPtr<ID3D11Texture2D> texture; Require(resource.As(&texture), "G-buffer texture");
        D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
        if (desc.Width != static_cast<UINT>(context.description.width) || desc.Height != static_cast<UINT>(context.description.height))
            throw std::runtime_error("G-buffer dimensions disagree with world target");
        found = true;
    }
    ComPtr<ID3D11Resource> resource; if (depth) depth->GetResource(&resource);
    Describe(resource.Get(), "GBufferDepth", context.frame, 90, Sampled(context.frame), context.dimensions + 6);
    if (!found || !depth) throw std::runtime_error("No G-buffer/depth bound at AfterGBuffer");
    // Depth is captured once, after transparents (CaptureInputs).
    context.stageMask.fetch_or(8); context.geometryDepth = resource; context.geometryReady = true;
}
static void CaptureInputs(SceneInputsContext& context)
{
    if (!context.geometryReady) throw std::runtime_error("Missing fresh AfterGBuffer event before input capture");
    context.geometryReady = false;
    CaptureDepth(context, 100);
    ComPtr<ID3D11RenderTargetView> target; context.pass.immediate->OMGetRenderTargets(1, &target, nullptr);
    ComPtr<ID3D11Resource> resource; if (target) target->GetResource(&resource);
    Describe(resource.Get(), "boundColorTarget", context.frame, 100, Sampled(context.frame));
    context.sceneColor.Reset(); if (resource) resource.As(&context.sceneColor);
    context.sceneColorFrame = context.sceneColor ? context.unityFrame : 0;
}
static void ValidateMotion(SceneInputsContext& context)
{
    // Unity's motion-vector blit source is bound at slot 0 when this event runs.
    ComPtr<ID3D11ShaderResourceView> source; context.pass.immediate->PSGetShaderResources(0, 1, &source);
    ComPtr<ID3D11Resource> resource; if (source) source->GetResource(&resource);
    Describe(resource.Get(), "motionBlitSourceSlot0", context.frame, 102, Sampled(context.frame), context.dimensions + 2);
    if (resource) context.stageMask.fetch_or(4);
}
static void __stdcall SceneInputsFrameEvent(int unityFrame, void* pointer)
{
    auto context = static_cast<SceneInputsContext*>(pointer);
    if (context && context->state.load() >= 0) context->unityFrame = unityFrame;
}
extern "C" void* __cdecl RiskOfRainUpscaling_SceneInputsGetFrameEvent() { return reinterpret_cast<void*>(&SceneInputsFrameEvent); }
bool SceneInputsReady(void* pointer, int frame, ID3D11Device* device) {
    auto c=static_cast<SceneInputsContext*>(pointer);
    return c && c->state.load()>=0 && c->pass.device.Get()==device && c->renderThread==GetCurrentThreadId() &&
        c->unityFrame==frame && (c->stageMask.load()&14)==14 &&
        c->dimensions[0]==c->description.width && c->dimensions[1]==c->description.height &&
        c->dimensions[2]==c->description.width && c->dimensions[3]==c->description.height;
}
ID3D11Texture2D* SceneInputsSceneColor(void* pointer, int frame, ID3D11Device* device) {
    auto c=static_cast<SceneInputsContext*>(pointer);
    return c && c->state.load()>=0 && c->pass.device.Get()==device && c->renderThread==GetCurrentThreadId() &&
        c->sceneColorFrame==frame ? c->sceneColor.Get() : nullptr;
}
int SceneInputsGetMetrics(void* pointer, SceneInputsMetrics* output)
{
    auto context = static_cast<SceneInputsContext*>(pointer);
    if (!context || !output) return -1;
    auto& d = context->dimensions;
    *output = {static_cast<int>(context->frame.load()),d[0].load(),d[1].load(),d[2].load(),d[3].load(),context->stageMask.load(),
        d[4].load(),d[5].load(),d[6].load(),d[7].load()};
    return 0;
}
static void __stdcall SceneInputsEvent(int stage, void* pointer)
{
    auto context = static_cast<SceneInputsContext*>(pointer);
    if (!context) return;
    if (stage == 199) { context->Reset(); context->state.store(-1, std::memory_order_release); return; }
    if (context->state.load() < 0) return;
    try
    {
        if (stage == 90) ProbeGeometry(*context);
        else if (stage == 100) CaptureInputs(*context);
        else if (stage == 102) ValidateMotion(*context);
        else throw std::runtime_error("Unknown scene input stage");
        context->state.store(1, std::memory_order_release);
    }
    catch (const std::exception& exception) { Log(ModLogLevel::Error, "SCENE_INPUTS_EVENT_FAILED stage=%d %s", stage, exception.what()); context->state = -2; }
}
extern "C" void* __cdecl RiskOfRainUpscaling_SceneInputsGetEvent() { return reinterpret_cast<void*>(&SceneInputsEvent); }
extern "C" int __cdecl RiskOfRainUpscaling_SceneInputsState(void* pointer) { return pointer ? static_cast<SceneInputsContext*>(pointer)->state.load(std::memory_order_acquire) : -2; }
extern "C" void __cdecl RiskOfRainUpscaling_SceneInputsDestroy(void* pointer)
{
    auto context = static_cast<SceneInputsContext*>(pointer);
    if (context && context->state.load(std::memory_order_acquire) == -1) delete context;
}
