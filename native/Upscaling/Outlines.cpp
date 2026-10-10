#include "Upscaling/UpscalerContextImpl.h"
#include "shaders/OutlineUpscale.h"
#include "shaders/OutlineWeightInput.h"
#include "shaders/OutlineResolve.h"
#include "shaders/OutlineResolveFallback.h"
#include "shaders/OutlineComposite.h"
#include "shaders/OutlineOcclusionDepth.h"
#include "shaders/OutlineSobelDepth.h"
#include "shaders/OutlineSobel.h"
#include <algorithm>
#include <cstring>
#include <initializer_list>

void CreateOutlines(UpscalerContext& c) {
    if(c.Upscaling()) {
        RequireUpscaler(c.device->CreatePixelShader(OutlineUpscaleShader,sizeof(OutlineUpscaleShader),nullptr,&c.outlineShader),"Outline mask upscale shader");
        const UINT constants[4]{static_cast<UINT>(c.desc.outputWidth),static_cast<UINT>(c.desc.outputHeight),0,0};
        D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=sizeof(constants);buffer.Usage=D3D11_USAGE_IMMUTABLE;buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA data{constants,0,0};
        RequireUpscaler(c.device->CreateBuffer(&buffer,&data,&c.outlineConstants),"Outline mask upscale constants");
    }
    if(c.Upscaling() && c.BeforePost()) {
        RequireUpscaler(c.device->CreatePixelShader(OutlineOcclusionDepthShader,sizeof(OutlineOcclusionDepthShader),nullptr,&c.occlusionDepthShader),"Outline occlusion depth shader");
        RequireUpscaler(c.device->CreatePixelShader(OutlineSobelDepthShader,sizeof(OutlineSobelDepthShader),nullptr,&c.sobelDepthShader),"Outline Sobel depth shader");
        RequireUpscaler(c.device->CreatePixelShader(OutlineSobelShader,sizeof(OutlineSobelShader),nullptr,&c.sobelShader),"Outline Sobel shader");
    }
    if(c.BeforePost()) {
        RequireUpscaler(c.device->CreatePixelShader(OutlineWeightInputShader,sizeof(OutlineWeightInputShader),nullptr,&c.weightInputShader),"Outline weight input shader");
        RequireUpscaler(c.device->CreatePixelShader(OutlineResolveShader,sizeof(OutlineResolveShader),nullptr,&c.resolveShader),"Outline resolve shader");
        RequireUpscaler(c.device->CreatePixelShader(OutlineResolveFallbackShader,sizeof(OutlineResolveFallbackShader),nullptr,&c.resolveFallbackShader),"Outline fallback resolve shader");
        RequireUpscaler(c.device->CreatePixelShader(OutlineCompositeShader,sizeof(OutlineCompositeShader),nullptr,&c.compositeShader),"Outline composite shader");
    }
}
// Upscaling only: edge-sharpened upscale of the render-sized outline mask into the display-sized
// one. A failure falls back to bilinear, so the composite never reads stale data.
static void __stdcall OutlineUpscaleEvent(int event, void* pointer) {
    auto c=static_cast<UpscalerContext*>(pointer); if(!c || event<=0 || !c->Upscaling() || c->state.load()==-1) return;
    std::lock_guard<std::mutex> guard(c->mutex);
    if(!c->outlineSource || !c->outlineTarget) return;
    try {
        if(c->renderThread && c->renderThread!=GetCurrentThreadId()) throw std::runtime_error("Outline callback changed render threads");
        auto saved=c->Scope();
        ProfileScope p(PROFILE_LABEL("outlines.maskUpscale"));
        if(c->outlineFailed) {Convert(*c,c->outlineSource.Get(),c->outlineTarget.Get(),c->upscaleShader.Get(),nullptr,true);return;}
        auto buffer=c->outlineConstants.Get();c->immediate->PSSetConstantBuffers(0,1,&buffer);
        Convert(*c,c->outlineSource.Get(),c->outlineTarget.Get(),c->outlineShader.Get(),nullptr,true);
        if(++c->outlines<=2) Log(ModLogLevel::Debug,"OUTLINE_UPSCALE unityFrame=%d render=%dx%d output=%dx%d",event,c->desc.width,c->desc.height,c->desc.outputWidth,c->desc.outputHeight);
    } catch(const std::exception& e) {
        c->outlineFailed=true;Log(ModLogLevel::Error,"OUTLINE_UPSCALE_FAILED unityFrame=%d %s; bilinear outline mask from now on",event,e.what());
        try {auto saved=c->Scope();c->immediate->ClearState();Convert(*c,c->outlineSource.Get(),c->outlineTarget.Get(),c->upscaleShader.Get(),nullptr,true);} catch(...) {}
    }
}
static bool OutlineMaskView(UpscalerContext& c, void* pointer, bool output, ComPtr<ID3D11Texture2D>& texture) {
    if(!pointer || FAILED(static_cast<ID3D11Resource*>(pointer)->QueryInterface(IID_PPV_ARGS(&texture)))) return false;
    ComPtr<ID3D11Device> owner; texture->GetDevice(&owner);
    D3D11_TEXTURE2D_DESC d{}; texture->GetDesc(&d);
    return owner.Get()==c.device.Get() && d.Width==static_cast<UINT>(output?c.desc.outputWidth:c.desc.width) && d.Height==static_cast<UINT>(output?c.desc.outputHeight:c.desc.height) &&
        d.SampleDesc.Count==1 && d.ArraySize==1 && d.MipLevels==1 && (d.BindFlags&(output?D3D11_BIND_RENDER_TARGET:D3D11_BIND_SHADER_RESOURCE)) &&
        (d.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS || d.Format==DXGI_FORMAT_R8G8B8A8_UNORM);
}
extern "C" int __cdecl RiskOfRainUpscaling_UpscalerSetOutlineMasks(void* pointer, void* source, void* target) {
    auto c=static_cast<UpscalerContext*>(pointer); if(!c || !c->Upscaling() || !c->outlineShader) return -1;
    try {
        ComPtr<ID3D11Texture2D> input, output;
        if(!OutlineMaskView(*c,source,false,input) || !OutlineMaskView(*c,target,true,output) || input==output) return -1;
        ComPtr<ID3D11ShaderResourceView> view; ComPtr<ID3D11RenderTargetView> render;
        D3D11_SHADER_RESOURCE_VIEW_DESC v{}; v.Format=DXGI_FORMAT_R8G8B8A8_UNORM; v.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels=1;
        RequireUpscaler(c->device->CreateShaderResourceView(input.Get(),&v,&view),"Outline mask SRV");
        D3D11_RENDER_TARGET_VIEW_DESC r{}; r.Format=DXGI_FORMAT_R8G8B8A8_UNORM; r.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
        RequireUpscaler(c->device->CreateRenderTargetView(output.Get(),&r,&render),"Outline mask RTV");
        std::lock_guard<std::mutex> guard(c->mutex);
        c->outlineSource=view; c->outlineTarget=render;
        Log("OUTLINE_MASKS render=%dx%d output=%dx%d; edge-sharpened outline mask upscale",c->desc.width,c->desc.height,c->desc.outputWidth,c->desc.outputHeight);
        return 0;
    } catch(const std::exception&) { return -1; }
}
extern "C" void* __cdecl RiskOfRainUpscaling_UpscalerGetOutlineUpscaleEvent() { return reinterpret_cast<void*>(&OutlineUpscaleEvent); }
// A Unity texture on our device with the given size, binding and format family.
static bool UnityTexture(UpscalerContext& c, void* pointer, int width, int height, UINT bind, std::initializer_list<DXGI_FORMAT> formats, ComPtr<ID3D11Texture2D>& texture) {
    if(!pointer || FAILED(static_cast<ID3D11Resource*>(pointer)->QueryInterface(IID_PPV_ARGS(&texture)))) return false;
    ComPtr<ID3D11Device> owner; texture->GetDevice(&owner);
    D3D11_TEXTURE2D_DESC d{}; texture->GetDesc(&d);
    return owner.Get()==c.device.Get() && d.Width==static_cast<UINT>(width) && d.Height==static_cast<UINT>(height) && d.SampleDesc.Count==1 &&
        d.ArraySize==1 && d.MipLevels==1 && (d.BindFlags&bind)==bind && std::find(formats.begin(),formats.end(),d.Format)!=formats.end();
}
extern "C" int __cdecl RiskOfRainUpscaling_UpscalerSetTemporalOutlines(void* pointer, void* mask, void* sobel, void* composite) {
    auto c=static_cast<UpscalerContext*>(pointer); if(!c || !c->BeforePost() || !c->compositeShader || c->state.load()!=0) return -1;
    try {
        ComPtr<ID3D11Texture2D> maskTexture, sobelTexture, compositeTexture;
        if(!UnityTexture(*c,mask,c->desc.width,c->desc.height,D3D11_BIND_SHADER_RESOURCE,{DXGI_FORMAT_R8G8B8A8_TYPELESS,DXGI_FORMAT_R8G8B8A8_UNORM},maskTexture) ||
           !UnityTexture(*c,sobel,c->desc.width,c->desc.height,D3D11_BIND_SHADER_RESOURCE,{DXGI_FORMAT_R8_TYPELESS,DXGI_FORMAT_R8_UNORM},sobelTexture) ||
           !UnityTexture(*c,composite,c->desc.outputWidth,c->desc.outputHeight,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET,{DXGI_FORMAT_R16G16B16A16_TYPELESS,DXGI_FORMAT_R16G16B16A16_FLOAT},compositeTexture) ||
           maskTexture==compositeTexture) return -1;
        ComPtr<ID3D11ShaderResourceView> maskView, sobelView, resolvedView; ComPtr<ID3D11RenderTargetView> compositeTarget, resolvedTarget; ComPtr<ID3D11Texture2D> resolved;
        D3D11_SHADER_RESOURCE_VIEW_DESC v{}; v.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels=1;
        v.Format=DXGI_FORMAT_R8G8B8A8_UNORM; RequireUpscaler(c->device->CreateShaderResourceView(maskTexture.Get(),&v,&maskView),"Temporal outline mask SRV");
        v.Format=DXGI_FORMAT_R8_UNORM; RequireUpscaler(c->device->CreateShaderResourceView(sobelTexture.Get(),&v,&sobelView),"Temporal outline Sobel SRV");
        D3D11_RENDER_TARGET_VIEW_DESC r{}; r.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; r.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
        RequireUpscaler(c->device->CreateRenderTargetView(compositeTexture.Get(),&r,&compositeTarget),"Outline composite RTV");
        OwnedTexture(*c,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET,resolved,true);
        RequireUpscaler(c->device->CreateShaderResourceView(resolved.Get(),nullptr,&resolvedView),"Resolved outline SRV");
        RequireUpscaler(c->device->CreateRenderTargetView(resolved.Get(),nullptr,&resolvedTarget),"Resolved outline RTV");
        std::lock_guard<std::mutex> guard(c->mutex);
        c->temporalMask=maskView; c->temporalSobel=sobelView; c->composite=compositeTexture; c->compositeTarget=compositeTarget;
        c->resolved=resolved; c->resolvedView=resolvedView; c->resolvedTarget=resolvedTarget; c->temporalOutlines=true;
        Log("TEMPORAL_OUTLINES requested render=%dx%d output=%dx%d; weight in the DLSS input alpha",c->desc.width,c->desc.height,c->desc.outputWidth,c->desc.outputHeight);
        return 0;
    } catch(const std::exception&) { return -1; }
}
// 1 once the feature accepted alpha upscaling, 0 otherwise.
extern "C" int __cdecl RiskOfRainUpscaling_UpscalerTemporalOutlinesActive(void* pointer) {
    auto c=static_cast<UpscalerContext*>(pointer); return c && c->temporalOutlines && c->state.load()>0?1:0;
}
// Composites the resolved outline into Unity's display-sized texture. Frames
// DLSS did not evaluate use the current render-sized weight, untemporal.
static void __stdcall OutlineCompositeEvent(int event, void* pointer) {
    auto c=static_cast<UpscalerContext*>(pointer); if(!c || event<=0 || c->state.load()==-1) return;
    std::lock_guard<std::mutex> guard(c->mutex);
    if(!c->temporalOutlines || !c->compositeTarget || c->compositeFailed) return;
    try {
        if(c->renderThread && c->renderThread!=GetCurrentThreadId()) throw std::runtime_error("Outline composite callback changed render threads");
        auto saved=c->Scope();
        ProfileScope p(PROFILE_LABEL("outlines.temporalComposite"));
        const bool current=c->outlineFrame==event;
        if(!current) Convert(*c,c->temporalMask.Get(),c->resolvedTarget.Get(),c->resolveFallbackShader.Get(),c->temporalSobel.Get(),true);
        c->immediate->CopyResource(c->decoded.Get(),c->composite.Get());
        Convert(*c,c->decodedView.Get(),c->compositeTarget.Get(),c->compositeShader.Get(),c->resolvedView.Get(),true);
        if(++c->composites<=2 || (!current && c->composites%600==0))
            Log(ModLogLevel::Debug,"TEMPORAL_OUTLINES_COMPOSITED unityFrame=%d weight=%s",event,current?"reconstructed":"current render frame");
    } catch(const std::exception& e) {
        // The texture still holds Unity's uncomposited copy.
        c->compositeFailed=true;Log(ModLogLevel::Error,"TEMPORAL_OUTLINES_FAILED unityFrame=%d %s; outlines off",event,e.what());
    }
}
extern "C" void* __cdecl RiskOfRainUpscaling_UpscalerGetOutlineCompositeEvent() { return reinterpret_cast<void*>(&OutlineCompositeEvent); }
// Before-post-processing upscaling: full-resolution outlines. Unity draws the
// highlighted objects' depth and the highlight mask at the display size (the
// fill pass testing against the occlusion depth); native code builds the
// display-sized Sobel depth and anti-aliased Sobel edges for the game's composite.
// The shader-readable view of a Unity depth texture, or DXGI_FORMAT_UNKNOWN.
static DXGI_FORMAT DepthViewFormat(DXGI_FORMAT format) {
    switch(format) {
        case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}
extern "C" int __cdecl RiskOfRainUpscaling_UpscalerSetFullResolutionOutlines(void* pointer, void* mask, void* objectDepth, void* occlusion, void* sobel, int reversedDepth) {
    auto c=static_cast<UpscalerContext*>(pointer);
    if(!c || !c->Upscaling() || !c->BeforePost() || !c->occlusionDepthShader || c->state.load()==-1 || reversedDepth<0 || reversedDepth>1) return -1;
    try {
        ComPtr<ID3D11Texture2D> maskTexture, objectTexture, occlusionTexture, sobelTexture, sobelDepth;
        const int w=c->desc.outputWidth, h=c->desc.outputHeight;
        if(!UnityTexture(*c,mask,w,h,D3D11_BIND_SHADER_RESOURCE,{DXGI_FORMAT_R8G8B8A8_TYPELESS,DXGI_FORMAT_R8G8B8A8_UNORM},maskTexture) ||
           !UnityTexture(*c,objectDepth,w,h,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_DEPTH_STENCIL,
               {DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_R24G8_TYPELESS,DXGI_FORMAT_R32G8X24_TYPELESS,DXGI_FORMAT_R16_TYPELESS},objectTexture) ||
           !UnityTexture(*c,occlusion,w,h,D3D11_BIND_RENDER_TARGET,{DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_R32_FLOAT},occlusionTexture) ||
           !UnityTexture(*c,sobel,w,h,D3D11_BIND_RENDER_TARGET,{DXGI_FORMAT_R16_TYPELESS,DXGI_FORMAT_R16_FLOAT},sobelTexture)) return -1;
        ComPtr<ID3D11ShaderResourceView> maskView, objectView, sobelDepthView; ComPtr<ID3D11RenderTargetView> occlusionTarget, sobelDepthTarget, sobelTarget;
        ComPtr<ID3D11Buffer> constants, sobelConstants;
        D3D11_SHADER_RESOURCE_VIEW_DESC v{}; v.Format=DXGI_FORMAT_R8G8B8A8_UNORM; v.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels=1;
        RequireUpscaler(c->device->CreateShaderResourceView(maskTexture.Get(),&v,&maskView),"Display outline mask SRV");
        D3D11_TEXTURE2D_DESC objectDesc{}; objectTexture->GetDesc(&objectDesc); v.Format=DepthViewFormat(objectDesc.Format);
        RequireUpscaler(c->device->CreateShaderResourceView(objectTexture.Get(),&v,&objectView),"Highlighted object depth SRV");
        D3D11_RENDER_TARGET_VIEW_DESC r{}; r.Format=DXGI_FORMAT_R32_FLOAT; r.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
        RequireUpscaler(c->device->CreateRenderTargetView(occlusionTexture.Get(),&r,&occlusionTarget),"Outline occlusion depth RTV");
        r.Format=DXGI_FORMAT_R16_FLOAT;
        RequireUpscaler(c->device->CreateRenderTargetView(sobelTexture.Get(),&r,&sobelTarget),"Outline Sobel RTV");
        OwnedTexture(*c,DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET,sobelDepth,true);
        RequireUpscaler(c->device->CreateShaderResourceView(sobelDepth.Get(),nullptr,&sobelDepthView),"Outline Sobel depth SRV");
        RequireUpscaler(c->device->CreateRenderTargetView(sobelDepth.Get(),nullptr,&sobelDepthTarget),"Outline Sobel depth RTV");
        const UINT values[4]{static_cast<UINT>(w),static_cast<UINT>(h),static_cast<UINT>(reversedDepth),0};
        D3D11_BUFFER_DESC buffer{}; buffer.ByteWidth=sizeof(values); buffer.Usage=D3D11_USAGE_IMMUTABLE; buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA data{values,0,0};
        RequireUpscaler(c->device->CreateBuffer(&buffer,&data,&constants),"Full-resolution outline constants");
        buffer.ByteWidth=16; buffer.Usage=D3D11_USAGE_DYNAMIC; buffer.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        RequireUpscaler(c->device->CreateBuffer(&buffer,nullptr,&sobelConstants),"Outline Sobel constants");
        std::lock_guard<std::mutex> guard(c->mutex);
        c->displayMask=maskView; c->objectDepth=objectView; c->occlusionTarget=occlusionTarget; c->sobelTarget=sobelTarget;
        c->sobelDepth=sobelDepthView; c->sobelDepthTarget=sobelDepthTarget; c->fullResolutionConstants=constants; c->sobelConstants=sobelConstants;
        c->reversedOutlineDepth=reversedDepth==1; c->fullResolutionOutlines=true; c->fullResolutionFailed=false;
        Log("FULL_RESOLUTION_OUTLINES render=%dx%d output=%dx%d reversedDepth=%d objectDepth=%u; mask, depth and 8x anti-aliased Sobel edges at the display size",
            c->desc.width,c->desc.height,w,h,reversedDepth,static_cast<unsigned>(objectDesc.Format));
        return 0;
    } catch(const std::exception&) { return -1; }
}
// 1 while full-resolution outlines are registered and have not failed (managed
// also requires a ready upscaler before drawing them).
extern "C" int __cdecl RiskOfRainUpscaling_UpscalerFullResolutionOutlinesActive(void* pointer) {
    auto c=static_cast<UpscalerContext*>(pointer);
    if(!c || c->state.load()==-1) return 0;
    std::lock_guard<std::mutex> guard(c->mutex);
    return c->fullResolutionOutlines && !c->fullResolutionFailed?1:0;
}
// The occlusion pass (before the fill pass), or the Sobel depth and edges
// (after it). A failure hides this frame's outline (occlusion nearer than
// anything, no edges); the next frame takes the render-sized fallback path.
static void OutlineDepth(int event, UpscalerContext* c, bool sobel) {
    if(!c || event<=0 || c->state.load()==-1) return;
    std::lock_guard<std::mutex> guard(c->mutex);
    if(!c->fullResolutionOutlines || c->fullResolutionFailed) return;
    try {
        if(c->renderThread && c->renderThread!=GetCurrentThreadId()) throw std::runtime_error("Outline depth callback changed render threads");
        auto saved=c->Scope();
        auto buffer=c->fullResolutionConstants.Get(); c->immediate->PSSetConstantBuffers(0,1,&buffer);
        if(!sobel) {
            ProfileScope p(PROFILE_LABEL("outlines.occlusionDepth"));
            Convert(*c,c->rawDepth.Get(),c->occlusionTarget.Get(),c->occlusionDepthShader.Get(),nullptr,true);
        } else {
            ProfileScope p(PROFILE_LABEL("outlines.sobel (depth + 8x edges)"));
            Convert(*c,c->rawDepth.Get(),c->sobelDepthTarget.Get(),c->sobelDepthShader.Get(),c->displayMask.Get(),true,c->objectDepth.Get());
            // Unity's _ZBufferParams.xy for this camera's near/far planes.
            const float n=c->outlineNear, f=c->outlineFar;
            const float values[4]{c->reversedOutlineDepth?f/n-1:1-f/n,c->reversedOutlineDepth?1:f/n,
                .002f*(c->desc.outputWidth+1),.002f*(c->desc.outputHeight+1)};
            D3D11_MAPPED_SUBRESOURCE mapped{};
            RequireUpscaler(c->immediate->Map(c->sobelConstants.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Outline Sobel constants map");
            std::memcpy(mapped.pData,values,sizeof(values)); c->immediate->Unmap(c->sobelConstants.Get(),0);
            ID3D11Buffer* both[2]{c->fullResolutionConstants.Get(),c->sobelConstants.Get()}; c->immediate->PSSetConstantBuffers(0,2,both);
            Convert(*c,c->sobelDepth.Get(),c->sobelTarget.Get(),c->sobelShader.Get(),c->displayMask.Get(),true);
        }
        if(++c->outlineDepths<=2) Log(ModLogLevel::Debug,"FULL_RESOLUTION_OUTLINE_PASS unityFrame=%d pass=%s near=%.3f far=%.1f",event,sobel?"sobel":"occlusion",c->outlineNear,c->outlineFar);
    } catch(const std::exception& e) {
        c->fullResolutionFailed=true; Log(ModLogLevel::Error,"FULL_RESOLUTION_OUTLINES_FAILED unityFrame=%d %s; outlines drawn from the render-sized mask from now on",event,e.what());
        try {
            auto saved=c->Scope(); c->immediate->ClearState();
            const float nearest=c->reversedOutlineDepth?1.f:0.f, occlusion[4]{nearest,nearest,nearest,nearest}, none[4]{};
            c->immediate->ClearRenderTargetView(c->occlusionTarget.Get(),occlusion);
            c->immediate->ClearRenderTargetView(c->sobelTarget.Get(),none);
        } catch(...) {}
    }
}
static void __stdcall OutlineOcclusionDepthEvent(int event, void* pointer) { OutlineDepth(event,static_cast<UpscalerContext*>(pointer),false); }
static void __stdcall OutlineSobelEvent(int event, void* pointer) { OutlineDepth(event,static_cast<UpscalerContext*>(pointer),true); }
extern "C" void* __cdecl RiskOfRainUpscaling_UpscalerGetOutlineOcclusionEvent() { return reinterpret_cast<void*>(&OutlineOcclusionDepthEvent); }
extern "C" void* __cdecl RiskOfRainUpscaling_UpscalerGetOutlineSobelEvent() { return reinterpret_cast<void*>(&OutlineSobelEvent); }
