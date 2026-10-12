#include "Upscaling/UpscalerContextImpl.h"
#include "Presentation/StreamlinePresentation.h"
#include "FrameGeneration/FrameGeneration.h"
#include "FrameGeneration/FrameBridge.h"
#include "FrameGeneration/FrameGenerationSpace.h"
#include "Diagnostics/Profiler.h"
#include "Core/FullscreenPass.h"
#include "shaders/Fullscreen.h"
#include "shaders/DepthInput.h"
#include "shaders/MotionInput.h"
#include "shaders/FgDepthInput.h"
#include "shaders/FgMotionInput.h"
#include "shaders/EncodeSrgb.h"
#include "shaders/DecodeSrgb.h"
#include "shaders/SanitizeHdr.h"
#include "shaders/PreserveAlpha.h"
#include "shaders/Upscale.h"
#include "shaders/UpscaleHdr.h"
#include <memory>
#include <stdexcept>
#include <cmath>
#include <cstring>
#include <algorithm>

void OwnedTexture(UpscalerContext& c, DXGI_FORMAT format, UINT bind, ComPtr<ID3D11Texture2D>& texture, bool output) {
    D3D11_TEXTURE2D_DESC d{}; d.Width=output?c.desc.outputWidth:c.desc.width; d.Height=output?c.desc.outputHeight:c.desc.height; d.MipLevels=d.ArraySize=1;
    d.Format=format; d.SampleDesc.Count=1; d.BindFlags=bind;
    RequireUpscaler(c.device->CreateTexture2D(&d,nullptr,&texture),"Typed DLAA texture");
}
// Raw depth is RFloat (or legacy ARGBFloat); raw motion is RGHalf (or legacy ARGBFloat).
// Returns true when the source has the compact single-purpose format.
static bool SourceView(UpscalerContext& c, void* pointer, bool depth, ComPtr<ID3D11ShaderResourceView>& view, ComPtr<ID3D11Texture2D>& texture) {
    if (!pointer) throw std::runtime_error("DLAA source null");
    ComPtr<ID3D11Device> owner; static_cast<ID3D11Resource*>(pointer)->GetDevice(&owner);
    RequireUpscaler(static_cast<ID3D11Resource*>(pointer)->QueryInterface(IID_PPV_ARGS(&texture)),"DLAA source texture");
    D3D11_TEXTURE2D_DESC d{}; texture->GetDesc(&d);
    const bool legacy=d.Format==DXGI_FORMAT_R32G32B32A32_FLOAT || d.Format==DXGI_FORMAT_R32G32B32A32_TYPELESS;
    const bool compact=depth?d.Format==DXGI_FORMAT_R32_FLOAT || d.Format==DXGI_FORMAT_R32_TYPELESS:d.Format==DXGI_FORMAT_R16G16_FLOAT || d.Format==DXGI_FORMAT_R16G16_TYPELESS;
    if(owner.Get()!=c.device.Get() || d.Width!=static_cast<UINT>(c.desc.width) || d.Height!=static_cast<UINT>(c.desc.height) || d.SampleDesc.Count!=1 ||
       d.ArraySize!=1 || d.MipLevels!=1 || (!legacy && !compact)) throw std::runtime_error("DLAA raw source device/format/size mismatch");
    D3D11_SHADER_RESOURCE_VIEW_DESC v{}; v.Format=LinearViewFormat(d.Format); v.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels=1;
    RequireUpscaler(c.device->CreateShaderResourceView(texture.Get(),&v,&view),"DLAA raw input SRV");
    return compact;
}
extern "C" void* __cdecl RiskOfRainUpscaling_UpscalerCreate(const UpscalerDescription* description) {
    static_assert(sizeof(UpscalerDescription)==72,"C description ABI");
    static_assert(sizeof(CameraFrame)==356,"C frame ABI");
    if(!description || description->size!=sizeof(*description) || description->version!=2 || !description->sceneInputs ||
       !description->color || !description->output || description->color==description->output || description->width<1 || description->height<1 ||
       description->outputWidth<description->width || description->outputHeight<description->height || description->quality<0 || description->quality>4 ||
       (description->quality==0)!=(description->outputWidth==description->width && description->outputHeight==description->height) ||
       description->placement<0 || description->placement>1) return nullptr;
    try {
        auto c=std::make_unique<UpscalerContext>(); c->desc=*description; c->upscaler=RequestedUpscaler(); c->preset=RequestedDlssPreset();
        RequireUpscaler(static_cast<ID3D11Resource*>(description->color)->QueryInterface(IID_PPV_ARGS(&c->source)),"DLAA color");
        RequireUpscaler(static_cast<ID3D11Resource*>(description->output)->QueryInterface(IID_PPV_ARGS(&c->output)),"DLAA output");
        c->source->GetDevice(&c->device);
        ComPtr<ID3D11Device> owner; c->output->GetDevice(&owner);
        D3D11_TEXTURE2D_DESC a{},b{}; c->source->GetDesc(&a); c->output->GetDesc(&b);
        if(owner.Get()!=c->device.Get() || a.Width!=static_cast<UINT>(description->width) || a.Height!=static_cast<UINT>(description->height) ||
           b.Width!=static_cast<UINT>(description->outputWidth) || b.Height!=static_cast<UINT>(description->outputHeight) || a.Format!=b.Format || a.SampleDesc.Count!=1 || b.SampleDesc.Count!=1 || a.ArraySize!=1 || b.ArraySize!=1 ||
           a.MipLevels!=1 || b.MipLevels!=1 || (a.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT && a.Format!=DXGI_FORMAT_R16G16B16A16_TYPELESS)) throw std::runtime_error("DLSS color/output require separate render/display-sized single-sample half-float textures");
        c->device->GetImmediateContext(&c->immediate);
        ComPtr<ID3D11Device1> device1; RequireUpscaler(c->device.As(&device1),"DLAA D3D11.1 device"); RequireUpscaler(c->immediate.As(&c->immediate1),"DLAA D3D11.1 context");
        auto level=c->device->GetFeatureLevel(); RequireUpscaler(device1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),nullptr,&c->privateState),"DLAA state bracket");
        c->depthCopy=SourceView(*c,description->depth,true,c->rawDepth,c->rawDepthTexture);
        SourceView(*c,description->motion,false,c->rawMotion,c->rawMotionTexture);
        OwnedTexture(*c,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_SHADER_RESOURCE,c->color);
        OwnedTexture(*c,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,c->result,true);
        OwnedTexture(*c,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET,c->merged,true);
        OwnedTexture(*c,DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET,c->depth);
        OwnedTexture(*c,DXGI_FORMAT_R16G16_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET,c->motion);
        OwnedTexture(*c,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET,c->encoded);
        OwnedTexture(*c,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET,c->decoded,true);
        OwnedTexture(*c,DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET,c->fgDepth);
        OwnedTexture(*c,DXGI_FORMAT_R16G16_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET,c->fgMotion);
        RequireUpscaler(c->device->CreateRenderTargetView(c->depth.Get(),nullptr,&c->depthTarget),"DLAA depth RTV");
        RequireUpscaler(c->device->CreateRenderTargetView(c->motion.Get(),nullptr,&c->motionTarget),"DLAA motion RTV");
        RequireUpscaler(c->device->CreateRenderTargetView(c->merged.Get(),nullptr,&c->mergedTarget),"DLAA merged output RTV");
        RequireUpscaler(c->device->CreateRenderTargetView(c->encoded.Get(),nullptr,&c->encodedTarget),"DLSS perceptual input RTV");
        RequireUpscaler(c->device->CreateRenderTargetView(c->decoded.Get(),nullptr,&c->decodedTarget),"DLSS linear result RTV");
        RequireUpscaler(c->device->CreateRenderTargetView(c->fgDepth.Get(),nullptr,&c->fgDepthTarget),"FG display-space depth RTV");
        RequireUpscaler(c->device->CreateRenderTargetView(c->fgMotion.Get(),nullptr,&c->fgMotionTarget),"FG display-space motion RTV");
        RequireUpscaler(c->device->CreateShaderResourceView(c->depth.Get(),nullptr,&c->depthView),"DLAA depth SRV");
        RequireUpscaler(c->device->CreateShaderResourceView(c->motion.Get(),nullptr,&c->motionView),"DLAA motion SRV");
        RequireUpscaler(c->device->CreateShaderResourceView(c->decoded.Get(),nullptr,&c->decodedView),"DLSS linear result SRV");
        RequireUpscaler(c->device->CreateShaderResourceView(c->result.Get(),nullptr,&c->resultView),"DLAA result SRV");
        RequireUpscaler(c->device->CreateShaderResourceView(c->color.Get(),nullptr,&c->colorView),"DLAA original alpha SRV");
        RequireUpscaler(c->device->CreateVertexShader(FullscreenShader,sizeof(FullscreenShader),nullptr,&c->vertex),"DLAA vertex shader");
        RequireUpscaler(c->device->CreatePixelShader(DepthInputShader,sizeof(DepthInputShader),nullptr,&c->depthShader),"DLAA depth shader");
        RequireUpscaler(c->device->CreatePixelShader(MotionInputShader,sizeof(MotionInputShader),nullptr,&c->motionShader),"DLAA motion shader");
        RequireUpscaler(c->device->CreatePixelShader(PreserveAlphaShader,sizeof(PreserveAlphaShader),nullptr,&c->alphaShader),"DLAA alpha shader");
        RequireUpscaler(c->device->CreatePixelShader(UpscaleShader,sizeof(UpscaleShader),nullptr,&c->upscaleShader),"DLSS fallback upscale shader");
        RequireUpscaler(c->device->CreatePixelShader(FgDepthInputShader,sizeof(FgDepthInputShader),nullptr,&c->fgDepthShader),"FG depth shader");
        RequireUpscaler(c->device->CreatePixelShader(FgMotionInputShader,sizeof(FgMotionInputShader),nullptr,&c->fgMotionShader),"FG motion shader");
        RequireUpscaler(c->device->CreatePixelShader(EncodeSrgbShader,sizeof(EncodeSrgbShader),nullptr,&c->encodeShader),"DLSS perceptual encode shader");
        RequireUpscaler(c->device->CreatePixelShader(DecodeSrgbShader,sizeof(DecodeSrgbShader),nullptr,&c->decodeShader),"DLSS linear decode shader");
        RequireUpscaler(c->device->CreatePixelShader(SanitizeHdrShader,sizeof(SanitizeHdrShader),nullptr,&c->sanitizeShader),"DLAA HDR sanitize shader");
        RequireUpscaler(c->device->CreatePixelShader(UpscaleHdrShader,sizeof(UpscaleHdrShader),nullptr,&c->upscaleHdrShader),"DLSS HDR fallback upscale shader");
        D3D11_SAMPLER_DESC sample{}; sample.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR; sample.MaxLOD=D3D11_FLOAT32_MAX;
        sample.AddressU=sample.AddressV=sample.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
        RequireUpscaler(c->device->CreateSamplerState(&sample,&c->sampler),"DLSS fallback sampler");
        CreateSharpening(*c);
        CreateOutlines(*c);
        Log("RCAS_STRENGTH strength=%.3f; post-DLAA gamma-2 RCAS with denoise; original alpha retained; before UI and FG",c->sharpness);
        D3D11_RASTERIZER_DESC r{}; r.FillMode=D3D11_FILL_SOLID; r.CullMode=D3D11_CULL_NONE; r.DepthClipEnable=TRUE;
        RequireUpscaler(c->device->CreateRasterizerState(&r,&c->rasterizer),"DLAA rasterizer");
        D3D11_BLEND_DESC blend{}; blend.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        blend.RenderTarget[0].SrcBlend=blend.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlend=blend.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ZERO;
        blend.RenderTarget[0].BlendOp=blend.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;
        RequireUpscaler(c->device->CreateBlendState(&blend,&c->blend),"DLAA blend state");
        D3D11_DEPTH_STENCIL_DESC ds{}; ds.DepthFunc=D3D11_COMPARISON_ALWAYS;
        RequireUpscaler(c->device->CreateDepthStencilState(&ds,&c->depthState),"DLAA depth state");
        Log("UPSCALER_CREATED render=%dx%d output=%dx%d quality=%d input=RGBA16F output=RGBA16F depth=R32F (%s) motion=RG16F; %s",
            description->width,description->height,description->outputWidth,description->outputHeight,description->quality,c->depthCopy?"copied":"converted",
            !c->BeforePost()?"end of effects":c->Upscaling()?"before post-processing (HDR scene color upscaled into the display-sized output; post-processing at display size; RCAS at end of effects)":
            "before post-processing (HDR scene color in place; RCAS at end of effects)");
        return c.release();
    } catch(const std::exception& e) { Log(ModLogLevel::Error, "UPSCALER_CREATE_FAILED %s",e.what()); return nullptr; }
}
extern "C" int __cdecl RiskOfRainUpscaling_UpscalerSubmitFrame(void* pointer, const CameraFrame* frame) {
    if(!pointer || !frame || frame->size!=sizeof(*frame) || frame->version!=1 || frame->frame<=0 || frame->reset<0 || frame->reset>1 ||
       frame->nearPlane<=0 || frame->farPlane<=frame->nearPlane || frame->fov<=0 || frame->aspect<=0 || frame->reversedDepth<0 || frame->reversedDepth>1) return -1;
    // All numeric camera fields must be finite before they enter the SDK.
    float numbers[84]; std::memcpy(numbers,frame->projection,sizeof(numbers));
    for(size_t i=0;i<84;++i) if(!std::isfinite(numbers[i])) return -1;
    auto c=static_cast<UpscalerContext*>(pointer); std::lock_guard<std::mutex> guard(c->mutex);
    if(c->state.load()<0 || c->frames.size()>=16 || frame->frame<=c->lastFrame || c->frames.count(frame->frame)) return -1;
    c->frames.emplace(frame->frame,*frame); return 0;
}
void Convert(UpscalerContext& c, ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target, ID3D11PixelShader* shader, ID3D11ShaderResourceView* second, bool output, ID3D11ShaderResourceView* third) {
    ID3D11ShaderResourceView* empty[3]{}; c.immediate->PSSetShaderResources(0,3,empty);
    c.immediate->OMSetRenderTargets(1,&target,nullptr); float factors[4]{}; c.immediate->OMSetBlendState(c.blend.Get(),factors,0xffffffff);
    c.immediate->OMSetDepthStencilState(c.depthState.Get(),0); c.immediate->RSSetState(c.rasterizer.Get());
    D3D11_VIEWPORT viewport{0,0,static_cast<float>(output?c.desc.outputWidth:c.desc.width),static_cast<float>(output?c.desc.outputHeight:c.desc.height),0,1};
    c.immediate->RSSetViewports(1,&viewport);
    auto sampler=c.sampler.Get(); c.immediate->PSSetSamplers(0,1,&sampler);
    c.immediate->IASetInputLayout(nullptr); c.immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c.immediate->VSSetShader(c.vertex.Get(),nullptr,0); c.immediate->PSSetShader(shader,nullptr,0);
    c.immediate->GSSetShader(nullptr,nullptr,0); c.immediate->HSSetShader(nullptr,nullptr,0); c.immediate->DSSetShader(nullptr,nullptr,0);
    ID3D11ShaderResourceView* sources[]{source,second,third};c.immediate->PSSetShaderResources(0,3,sources); c.immediate->Draw(3,0);
    c.immediate->PSSetShaderResources(0,3,empty); c.immediate->OMSetRenderTargets(0,nullptr,nullptr);
}
// The render-sized HDR scene target recorded after transparents, validated for
// this frame, or null. Never throws, so failure paths can use it.
static ID3D11Texture2D* SceneFor(UpscalerContext& c, int frame) {
    auto scene=SceneInputsSceneColor(c.desc.sceneInputs,frame,c.device.Get());
    if(!scene) return nullptr;
    ComPtr<ID3D11Device> owner; scene->GetDevice(&owner);
    D3D11_TEXTURE2D_DESC d{}; scene->GetDesc(&d);
    return owner.Get()==c.device.Get() && d.Width==static_cast<UINT>(c.desc.width) && d.Height==static_cast<UINT>(c.desc.height) && d.SampleDesc.Count==1 &&
        d.ArraySize==1 && d.MipLevels==1 && (d.Format==DXGI_FORMAT_R16G16B16A16_FLOAT || d.Format==DXGI_FORMAT_R16G16B16A16_TYPELESS) ? scene : nullptr;
}
// The original scene must still reach Unity when DLSS cannot write this frame.
// Before post-processing, DLAA writes the scene only after a full evaluation, so
// leaving it untouched is the fallback. Lower-resolution DLSS feeds the post
// stack from the output, so it gets a bilinear, sanitized copy of the scene.
static void Fallback(UpscalerContext& c, ID3D11Texture2D* scene=nullptr) {
    ProfileScope p(PROFILE_LABEL("upscale.fallback (no reconstruction this frame)"));
    if(c.BeforePost()) {
        if(!c.Upscaling() || !scene) return;
        c.immediate->CopyResource(c.color.Get(),scene);
        Convert(c,c.colorView.Get(),c.mergedTarget.Get(),c.upscaleHdrShader.Get(),nullptr,true);
        c.immediate->CopyResource(c.output.Get(),c.merged.Get());
        return;
    }
    if(!c.Upscaling()) {c.immediate->CopyResource(c.output.Get(),c.source.Get());return;}
    c.immediate->CopyResource(c.color.Get(),c.source.Get());
    Convert(c,c.colorView.Get(),c.mergedTarget.Get(),c.upscaleShader.Get(),nullptr,true);
    c.immediate->CopyResource(c.output.Get(),c.merged.Get());
}
static UpscalerOptions Options(const UpscalerContext& c) {
    return {c.desc.quality,static_cast<UINT>(c.desc.outputWidth),static_cast<UINT>(c.desc.outputHeight),c.BeforePost(),c.temporalOutlines,c.preset};
}
static void __stdcall EvaluateEvent(int event, void* pointer) {
    auto c=static_cast<UpscalerContext*>(pointer); if(!c || c->state.load()==-1) return;
    std::lock_guard<std::mutex> guard(c->mutex);
    std::lock_guard<std::recursive_mutex> sdkGuard(streamlineMutex);
    try {
        if(c->renderThread && c->renderThread!=GetCurrentThreadId()) throw std::runtime_error("DLAA callback changed render threads");
        c->renderThread=GetCurrentThreadId();
        auto saved=c->Scope();
        if(event==-1) {
            if(c->ownsFeature) {
                auto released=c->feature->Release();
                Log("UPSCALER_RETIRED lastFrame=%d evaluations=%d resets=%d longestResetRun=%d sdkPresentedFrames=%u %s",c->lastFrame,c->evaluations,c->resets,c->longestResetRun,c->feature->PresentedFrames(),released.c_str());
                c->ownsFeature=false;
            }
            c->frames.clear(); c->state=-1; return;
        }
        if(event==-2) {
            if(c->state.load()!=0) return;
            std::string unavailable;
            c->feature=CreateUpscalerFeature(c->device.Get(),c->upscaler,unavailable);
            if(!c->feature) {
                // Unsupported adapters keep the fallback image without a failure report.
                c->state=-2;
                Log(ModLogLevel::Warning,"UPSCALER_UNAVAILABLE %s; reconstruction off, original scene kept",unavailable.c_str());
                return;
            }
            c->kind=c->feature->Kind();
            const char* names[]{"Auto","DLSS","FSR3/4 DX12","FSR 3 DX11"};
            Log("UPSCALER_SELECTED selected=%s preference=%s dlssPreset=%d (0 = NVIDIA default per mode)",names[c->kind.load()],names[static_cast<int>(c->upscaler)],c->preset);
            auto options=Options(*c);
            auto settings=c->feature->Sizes(options);
            const UINT width=static_cast<UINT>(c->desc.width), height=static_cast<UINT>(c->desc.height);
            Log("UPSCALER_SIZES quality=%d output=%dx%d optimal=%ux%u min=%ux%u max=%ux%u requested=%ux%u",c->desc.quality,c->desc.outputWidth,c->desc.outputHeight,
                settings.optimalWidth,settings.optimalHeight,settings.minWidth,settings.minHeight,settings.maxWidth,settings.maxHeight,width,height);
            if(!c->Upscaling()) {
                if(settings.optimalWidth!=width || settings.optimalHeight!=height) throw std::runtime_error("DLAA optimal settings are not 100 percent");
            } else {
                // DLSS accepts any render size in the reported range. A zero range
                // means the plugin reported only its optimal size.
                UINT minWidth=settings.minWidth?settings.minWidth:settings.optimalWidth, maxWidth=settings.maxWidth?settings.maxWidth:settings.optimalWidth;
                UINT minHeight=settings.minHeight?settings.minHeight:settings.optimalHeight, maxHeight=settings.maxHeight?settings.maxHeight:settings.optimalHeight;
                if(width<minWidth || width>maxWidth || height<minHeight || height>maxHeight) throw std::runtime_error("DLSS render size is outside the supported range for this quality mode");
            }
            // Without alpha reconstruction the outlines keep their non-temporal path.
            c->feature->Configure(options); c->temporalOutlines=options.alpha;
            c->ownsFeature=true; c->state=1;
            if(c->temporalOutlines) Log("TEMPORAL_OUTLINES alpha upscaling on; outline weight reconstructed by %s",c->Upscaling()?"DLSS":"DLAA");
            // Backends that take display-encoded LDR color (DLSS, FSR3/4 DX12) get the
            // end-of-effects input encoded as sRGB around evaluation and decoded afterward.
            c->perceptual=!c->BeforePost() && c->feature->PerceptualLdr();
            if(c->BeforePost())
                Log("UPSCALER_READY quality=%d render=%dx%d output=%dx%d placement=before-post-processing pre-tonemap HDR input=linear (NaN/Inf sanitized) write=%s autoExposure=1 motion=current-to-previous UV",
                    c->desc.quality,c->desc.width,c->desc.height,c->desc.outputWidth,c->desc.outputHeight,c->Upscaling()?"display-sized output":"scene in place");
            else Log("UPSCALER_READY quality=%d render=%dx%d output=%dx%d placement=end-of-effects post-tonemap SDR input=%s autoExposure=1 motion=current-to-previous UV",
                c->desc.quality,c->desc.width,c->desc.height,c->desc.outputWidth,c->desc.outputHeight,c->perceptual?"sRGB-encoded":"linear");
            return;
        }
        if(event<=0) return;
        // The output is always initialized, even when a frame/SDK check fails.
        if(c->state.load()<1) {Fallback(*c,SceneFor(*c,event));return;}
        c->feature->BeginFrame();
        auto found=c->frames.find(event); if(found==c->frames.end()) throw std::runtime_error("Missing immutable camera packet for this frame");
        auto f=found->second; c->frames.erase(c->frames.begin(),std::next(found));
        c->outlineNear=f.nearPlane; c->outlineFar=f.farPlane;
        if(event<=c->lastFrame || !SceneInputsReady(c->desc.sceneInputs,event,c->device.Get())) throw std::runtime_error("Depth/motion are not fresh inputs for this scene frame");
        // Before post-processing, the HDR scene target is the input. DLAA also
        // writes it in place; upscaling writes the display-sized output.
        ID3D11Texture2D* scene=nullptr;
        if(c->BeforePost()) {
            scene=SceneFor(*c,event);
            if(!scene) throw std::runtime_error("Scene color after transparents is unavailable or not a render-sized single-sample RGBA16F HDR target for this frame");
        }
        bool reset=f.reset || event!=c->lastFrame+1;
        if(event!=c->lastFrame+1) Log("UPSCALER_HISTORY_RESET unityFrame=%d previousEvaluatedFrame=%d reason=%s",event,c->lastFrame,c->lastFrame?"evaluation-gap":"first-evaluation");
        // Profiling: one GPU/CPU scope per step of the evaluation.
        ProfileScope evaluation(PROFILE_LABEL("upscale.evaluate (total)"));
        {ProfileScope p(PROFILE_LABEL("upscale.colorCopy"));c->immediate->CopyResource(c->color.Get(),scene?scene:c->source.Get());}
        const bool outlines=scene && c->temporalOutlines;
        {
            ProfileScope p(outlines?PROFILE_LABEL("upscale.colorPrep (outline weight + sanitize)"):scene?PROFILE_LABEL("upscale.colorPrep (HDR sanitize)"):PROFILE_LABEL("upscale.colorPrep (sRGB encode)"));
            if(outlines) Convert(*c,c->colorView.Get(),c->encodedTarget.Get(),c->weightInputShader.Get(),c->temporalMask.Get(),false,c->temporalSobel.Get());
            else if(scene) Convert(*c,c->colorView.Get(),c->encodedTarget.Get(),c->sanitizeShader.Get());
            else if(c->perceptual) Convert(*c,c->colorView.Get(),c->encodedTarget.Get(),c->encodeShader.Get());
        }
        // An RFloat raw depth is already the R32F DLSS input; copy it into our typed texture.
        if(c->depthCopy) {ProfileScope p(PROFILE_LABEL("upscale.depthCopy (R32F)"));c->immediate->CopyResource(c->depth.Get(),c->rawDepthTexture.Get());}
        else {ProfileScope p(PROFILE_LABEL("upscale.depthConvert (RGBA32F -> R32F)"));Convert(*c,c->rawDepth.Get(),c->depthTarget.Get(),c->depthShader.Get());}
        // Unity's current-minus-previous UV becomes current-to-previous: negated.
        {ProfileScope p(PROFILE_LABEL("upscale.motionConvert (negate -> RG16F)"));Convert(*c,c->rawMotion.Get(),c->motionTarget.Get(),c->motionShader.Get());}
        const UpscalerFrame frame{event,reset,!scene && c->perceptual,&f,scene || c->perceptual?c->encoded.Get():c->color.Get(),c->depth.Get(),c->motion.Get(),c->result.Get(),
            static_cast<UINT>(c->desc.width),static_cast<UINT>(c->desc.height),static_cast<UINT>(c->desc.outputWidth),static_cast<UINT>(c->desc.outputHeight)};
        {
            ProfileScope p(c->kind.load()==2?PROFILE_LABEL("upscale.feature FSR3/4 DX12 (D3D11 side only; FSR runs on D3D12)"):
                c->kind.load()==3?PROFILE_LABEL("upscale.feature FSR 3 DX11"):
                c->temporalOutlines?PROFILE_LABEL("upscale.feature DLSS (with alpha upscaling)"):PROFILE_LABEL("upscale.feature DLSS"));
            c->feature->Evaluate(c->immediate.Get(),frame);
            // Leave no SDK output bindings active when copying to Unity's sampled texture.
            c->immediate->ClearState();
        }
        if(scene) {
            // The post stack reads this target next. RCAS needs tonemapped color,
            // so it runs later through the sharpen event.
            {ProfileScope p(PROFILE_LABEL("upscale.preserveAlpha merge"));Convert(*c,c->resultView.Get(),c->mergedTarget.Get(),c->alphaShader.Get(),c->colorView.Get(),true);}
            {ProfileScope p(PROFILE_LABEL("upscale.outputCopy"));c->immediate->CopyResource(c->Upscaling()?c->output.Get():scene,c->merged.Get());}
            if(outlines) {ProfileScope p(PROFILE_LABEL("upscale.outlineResolve"));Convert(*c,c->resultView.Get(),c->resolvedTarget.Get(),c->resolveShader.Get(),c->temporalMask.Get(),true);c->outlineFrame=event;}
        } else {
            UpdateSharpness(*c);
            // RCAS and the rest of Unity's chain expect the original linear values.
            if(c->perceptual) {ProfileScope p(PROFILE_LABEL("upscale.sRGB decode"));Convert(*c,c->resultView.Get(),c->decodedTarget.Get(),c->decodeShader.Get(),nullptr,true);}
            auto linear=c->perceptual?c->decodedView.Get():c->resultView.Get();
            if(c->sharpness>0) {
                ProfileScope p(PROFILE_LABEL("upscale.RCAS (end of effects)"));
                auto buffer=c->rcasConstants.Get();c->immediate->PSSetConstantBuffers(0,1,&buffer);
                Convert(*c,linear,c->mergedTarget.Get(),c->rcasShader.Get(),c->colorView.Get(),true);
            } else {ProfileScope p(PROFILE_LABEL("upscale.preserveAlpha merge"));Convert(*c,linear,c->mergedTarget.Get(),c->alphaShader.Get(),c->colorView.Get(),true);}
            {ProfileScope p(PROFILE_LABEL("upscale.outputCopy"));c->immediate->CopyResource(c->output.Get(),c->merged.Get());}
        }
        f.reset=reset?1:0;
        if(FrameGenerationWanted()) {
            // DLAA's inputs stay in Unity's flipped layout; FG gets top-down copies.
            ProfileScope p(PROFILE_LABEL("fg.inputConvert (depth + motion flip)"));
            Convert(*c,c->depthView.Get(),c->fgDepthTarget.Get(),c->fgDepthShader.Get());
            Convert(*c,c->motionView.Get(),c->fgMotionTarget.Get(),c->fgMotionShader.Get());
            FrameBridgeSubmitInputs(FrameGenerationPacket(f),c->fgDepth.Get(),c->fgMotion.Get(),c->desc.outputWidth,c->desc.outputHeight);
        }
        c->lastFrame=event; ++c->evaluations; c->state=2;
        c->resetRun=reset?c->resetRun+1:0;
        if(reset)++c->resets;
        c->longestResetRun=(std::max)(c->longestResetRun,c->resetRun);
        if(c->evaluations<=4 || c->evaluations%300==0 || reset)
            Log(ModLogLevel::Debug, "UPSCALER_EVALUATED unityFrame=%d count=%d reset=%d jitter=%.6f,%.6f render=%dx%d output=%dx%d depthInverted=%d motionScale=%.3f,%.3f resets=%d longestResetRun=%d sdkPresentedFrames=%u",event,c->evaluations,reset,f.jitterX,f.jitterY,c->desc.width,c->desc.height,c->desc.outputWidth,c->desc.outputHeight,f.reversedDepth,f.motionScaleX,f.motionScaleY,c->resets,c->longestResetRun,c->feature->PresentedFrames());
    } catch(const std::exception& e) {
        // A partial SDK write must never replace the fallback image.
        if(event>0) { auto saved=c->Scope(); c->immediate->ClearState(); Fallback(*c,SceneFor(*c,event)); }
        c->state=event==-1?-1:-2;
        Log(ModLogLevel::Error, "UPSCALER_EVENT_FAILED event=%d %s; %s; DLSS latched off",event,e.what(),
            c->BeforePost()?(c->Upscaling()?"scene left unmodified; bilinear HDR upscale given to post-processing":"scene left unmodified before post-processing"):
            c->Upscaling()?"original scene copied with bilinear upscale":"original scene copied");
    }
}
// Before-post-processing upscaling, on frames the camera did not prepare (second
// renders, not yet ready, latched off): history is untouched.
static void __stdcall FallbackEvent(int event, void* pointer) {
    auto c=static_cast<UpscalerContext*>(pointer); if(!c || event<=0 || !c->BeforePost() || !c->Upscaling() || c->state.load()==-1) return;
    std::lock_guard<std::mutex> guard(c->mutex);
    try {
        if(c->renderThread && c->renderThread!=GetCurrentThreadId()) throw std::runtime_error("Fallback callback changed render threads");
        auto saved=c->Scope();
        auto scene=SceneFor(*c,event);
        if(!scene) throw std::runtime_error("Scene color after transparents is unavailable for this frame");
        Fallback(*c,scene);
        if(++c->fallbacks<=2 || c->fallbacks%600==0) Log(ModLogLevel::Warning, "UPSCALER_FALLBACK unityFrame=%d count=%d state=%d; bilinear HDR upscale before post-processing",event,c->fallbacks,c->state.load());
    } catch(const std::exception& e) {
        if(!c->fallbackFailed) {c->fallbackFailed=true;Log(ModLogLevel::Error, "UPSCALER_FALLBACK_FAILED unityFrame=%d %s; previous output kept",event,e.what());}
    }
}
extern "C" void* __cdecl RiskOfRainUpscaling_UpscalerGetEvaluateEvent() { return reinterpret_cast<void*>(&EvaluateEvent); }
extern "C" void* __cdecl RiskOfRainUpscaling_UpscalerGetFallbackEvent() { return reinterpret_cast<void*>(&FallbackEvent); }
// The running super-resolution backend: 0 none yet, 1 DLSS, 2 FSR.
extern "C" int __cdecl RiskOfRainUpscaling_UpscalerKind(void* pointer) { return pointer?static_cast<UpscalerContext*>(pointer)->kind.load():0; }
extern "C" int __cdecl RiskOfRainUpscaling_UpscalerState(void* pointer) { return pointer?static_cast<UpscalerContext*>(pointer)->state.load():-2; }
extern "C" void __cdecl RiskOfRainUpscaling_UpscalerDestroy(void* pointer) { auto c=static_cast<UpscalerContext*>(pointer); if(c && c->state.load()==-1) delete c; }
