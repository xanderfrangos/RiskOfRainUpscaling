#include "Upscaling/UpscalerContextImpl.h"
#include "shaders/Rcas.h"
#include <cmath>

// Negative means no managed override; standalone fixtures retain their INI settings.
static std::atomic<float> requestedSharpness{-1.f};
extern "C" int __cdecl RiskOfRainUpscaling_SetSharpness(float value) {
    if(!std::isfinite(value) || value<0.f || value>1.f)return -1;
    requestedSharpness.store(value);return 0;
}
void CreateSharpening(UpscalerContext& c) {
    // The plugin sends the saved Sharpening value before the first context.
    const float requested=requestedSharpness.load();
    c.sharpness=requested>=0?requested:DefaultRcasSharpness;
    RequireUpscaler(c.device->CreatePixelShader(RcasShader,sizeof(RcasShader),nullptr,&c.rcasShader),"RCAS shader");
    RcasConstants constants{c.sharpness,c.desc.outputWidth,c.desc.outputHeight};
    D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=sizeof(constants);buffer.Usage=D3D11_USAGE_DEFAULT;buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA data{&constants,0,0};
    RequireUpscaler(c.device->CreateBuffer(&buffer,&data,&c.rcasConstants),"RCAS constants");
}
void UpdateSharpness(UpscalerContext& c) {
    const float requested=requestedSharpness.load();
    if(requested<0 || requested==c.sharpness) return;
    c.sharpness=requested;
    RcasConstants rcas{c.sharpness,c.desc.outputWidth,c.desc.outputHeight};
    c.immediate->UpdateSubresource(c.rcasConstants.Get(),0,nullptr,&rcas,0,0);
    Log("RCAS_CHANGED strength=%.3f; applied on render thread without resetting DLSS history",c.sharpness);
}
// Before-post-processing placement: the tail copies the tonemapped scene into
// color (DLAA) or the display-sized output (upscaling); RCAS writes output.
// A failure still gives Unity an unsharpened copy.
static void __stdcall SharpenEvent(int event, void* pointer) {
    auto c=static_cast<UpscalerContext*>(pointer); if(!c || event<=0 || !c->BeforePost() || c->state.load()==-1) return;
    std::lock_guard<std::mutex> guard(c->mutex);
    try {
        if(c->renderThread && c->renderThread!=GetCurrentThreadId()) throw std::runtime_error("RCAS callback changed render threads");
        auto saved=c->Scope();
        UpdateSharpness(*c);
        ProfileScope p(c->sharpness>0?PROFILE_LABEL("rcas.sharpen (copy + RCAS + copy)"):PROFILE_LABEL("rcas.off (copy only)"));
        if(c->Upscaling()) {
            // Output already holds the post-processed display image; 0 leaves it.
            if(c->sharpness>0) {
                c->immediate->CopyResource(c->decoded.Get(),c->output.Get());
                auto buffer=c->rcasConstants.Get();c->immediate->PSSetConstantBuffers(0,1,&buffer);
                Convert(*c,c->decodedView.Get(),c->mergedTarget.Get(),c->rcasShader.Get(),c->decodedView.Get(),true);
                c->immediate->CopyResource(c->output.Get(),c->merged.Get());
            }
        } else if(c->sharpness>0) {
            c->immediate->CopyResource(c->color.Get(),c->source.Get());
            auto buffer=c->rcasConstants.Get();c->immediate->PSSetConstantBuffers(0,1,&buffer);
            Convert(*c,c->colorView.Get(),c->mergedTarget.Get(),c->rcasShader.Get(),c->colorView.Get(),true);
            c->immediate->CopyResource(c->output.Get(),c->merged.Get());
        } else c->immediate->CopyResource(c->output.Get(),c->source.Get());
        if(++c->sharpened<=2) Log(ModLogLevel::Debug, "RCAS_APPLIED unityFrame=%d strength=%.3f; after post-processing, before HUD",event,c->sharpness);
    } catch(const std::exception& e) {
        // Upscaling writes output only in its final copy, so it is still unsharpened.
        auto saved=c->Scope(); c->immediate->ClearState(); if(!c->Upscaling()) c->immediate->CopyResource(c->output.Get(),c->source.Get());
        if(!c->sharpenFailed) {c->sharpenFailed=true;Log(ModLogLevel::Error, "RCAS_FAILED unityFrame=%d %s; unsharpened scene kept",event,e.what());}
    }
}
extern "C" void* __cdecl RiskOfRainUpscaling_UpscalerGetSharpenEvent() { return reinterpret_cast<void*>(&SharpenEvent); }
