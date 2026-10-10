#include "FrameGeneration/HudOpacity.h"
#include "Diagnostics/Profiler.h"
#include "shaders/Fullscreen.h"
#include "shaders/UiAlpha.h"
#include "shaders/UiAlphaFlipped.h"
#include <array>
#include <map>
#include <mutex>
#include <functional>
#include <stdexcept>

namespace {
void RequireHud(HRESULT hr){if(FAILED(hr))throw std::runtime_error("HUD opacity resource/state creation failed");}
struct CaptureDraw {
    ID3D11DeviceContext* context{};
    virtual void Replay(const std::function<void()>&)=0;
    virtual void Invalidate(const char*)=0;
    virtual bool Targets(ID3D11Resource*) const=0;
    virtual ~CaptureDraw()=default;
};
thread_local CaptureDraw* current=nullptr;
// Hooks are process resident, like the existing producer-view hooks. Keep the
// previous function per vtable slot so WARP/hardware and existing hooks coexist.
auto& HookMutex(){static auto value=new std::mutex;return *value;}
auto& Originals(){static auto value=new std::map<void**,void*>;return *value;}
template<class Fn> Fn Original(ID3D11DeviceContext* c,int index){
    std::lock_guard<std::mutex> guard(HookMutex());
    return reinterpret_cast<Fn>(Originals().at(*reinterpret_cast<void***>(c)+index));
}
template<int Index,class... Args> void STDMETHODCALLTYPE DrawHud(ID3D11DeviceContext* c,Args... args){
    using Fn=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,Args...);
    auto original=Original<Fn>(c,Index);
    if(current && current->context==c)current->Replay([&]{original(c,args...);});
    original(c,args...);
}
void STDMETHODCALLTYPE ExecuteHud(ID3D11DeviceContext* c,ID3D11CommandList* list,BOOL restore){
    using Fn=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11CommandList*,BOOL);
    if(current && current->context==c)current->Invalidate("deferred HUD commands");
    Original<Fn>(c,58)(c,list,restore);
}
void STDMETHODCALLTYPE ClearHud(ID3D11DeviceContext* c,ID3D11RenderTargetView* view,const FLOAT color[4]){
    using Fn=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11RenderTargetView*,const FLOAT*);
    if(current && current->context==c && view){ComPtr<ID3D11Resource> resource;view->GetResource(&resource);if(current->Targets(resource.Get()))current->Invalidate("HUD target cleared during capture");}
    Original<Fn>(c,50)(c,view,color);
}
template<int Index> void STDMETHODCALLTYPE HudQuery(ID3D11DeviceContext* c,ID3D11Asynchronous* query){
    using Fn=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Asynchronous*);
    if(current && current->context==c && !ProfilerIssuingQuery())current->Invalidate("HUD GPU query");
    Original<Fn>(c,Index)(c,query);
}
template<int Index,class... Args> void STDMETHODCALLTYPE WriteHud(ID3D11DeviceContext* c,ID3D11Resource* destination,Args... args){
    using Fn=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,Args...);
    if(current && current->context==c && current->Targets(destination))current->Invalidate("HUD target modified outside draw capture");
    Original<Fn>(c,Index)(c,destination,args...);
}
void HookDraws(ID3D11DeviceContext* c){
    std::lock_guard<std::mutex> guard(HookMutex());
    auto patch=[&](int index,void* hook){
        auto slot=*reinterpret_cast<void***>(c)+index;if(*slot==hook)return;
        DWORD old{};RequireHud(VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&old)?S_OK:HRESULT_FROM_WIN32(GetLastError()));
        Originals()[slot]=*slot;InterlockedExchangePointer(slot,hook);
        DWORD ignored{};VirtualProtect(slot,sizeof(void*),old,&ignored);
    };
    patch(12,reinterpret_cast<void*>(&DrawHud<12,UINT,UINT,INT>));
    patch(13,reinterpret_cast<void*>(&DrawHud<13,UINT,UINT>));
    patch(20,reinterpret_cast<void*>(&DrawHud<20,UINT,UINT,UINT,INT,UINT>));
    patch(21,reinterpret_cast<void*>(&DrawHud<21,UINT,UINT,UINT,UINT>));
    patch(38,reinterpret_cast<void*>(&DrawHud<38>));
    patch(39,reinterpret_cast<void*>(&DrawHud<39,ID3D11Buffer*,UINT>));
    patch(40,reinterpret_cast<void*>(&DrawHud<40,ID3D11Buffer*,UINT>));
    patch(58,reinterpret_cast<void*>(&ExecuteHud));
    patch(50,reinterpret_cast<void*>(&ClearHud));
    patch(27,reinterpret_cast<void*>(&HudQuery<27>));
    patch(28,reinterpret_cast<void*>(&HudQuery<28>));
    patch(47,reinterpret_cast<void*>(&WriteHud<47,ID3D11Resource*>));
    patch(46,reinterpret_cast<void*>(&WriteHud<46,UINT,UINT,UINT,UINT,ID3D11Resource*,UINT,const D3D11_BOX*>));
    patch(48,reinterpret_cast<void*>(&WriteHud<48,UINT,const D3D11_BOX*,const void*,UINT,UINT>));
}
}

struct HudOpacity::Impl : CaptureDraw {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediate;
    ComPtr<ID3D11DeviceContext1> context1;
    ComPtr<ID3DDeviceContextState> extraction;
    ComPtr<ID3D11Texture2D> color,alpha,target;
    ComPtr<ID3D11RenderTargetView> colorView,alphaView;
    ComPtr<ID3D11ShaderResourceView> colorInput;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel,flippedPixel;
    ComPtr<ID3D11BlendState> opacityBlend;
    struct DepthPair {ComPtr<ID3D11DepthStencilState> original,readOnly;};
    std::map<ID3D11DepthStencilState*,DepthPair> depths;
    UINT width,height;
    int frame=0,finished=0;
    bool valid=false,flipRows=false;
    const char* reason="no HUD capture";
    Impl(ID3D11Device* d,ID3D11DeviceContext* c,UINT w,UINT h):device(d),immediate(c),width(w),height(h){
        context=c;RequireHud(c->QueryInterface(IID_PPV_ARGS(&context1)));
        ComPtr<ID3D11Device1> device1;RequireHud(d->QueryInterface(IID_PPV_ARGS(&device1)));
        auto level=d->GetFeatureLevel();RequireHud(device1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),nullptr,&extraction));
        D3D11_TEXTURE2D_DESC t{};t.Width=w;t.Height=h;t.ArraySize=t.MipLevels=1;t.SampleDesc.Count=1;
        t.Format=DXGI_FORMAT_R8G8B8A8_UNORM;t.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        RequireHud(d->CreateTexture2D(&t,nullptr,&color));RequireHud(d->CreateRenderTargetView(color.Get(),nullptr,&colorView));
        RequireHud(d->CreateShaderResourceView(color.Get(),nullptr,&colorInput));
        t.Format=DXGI_FORMAT_R8_UNORM;RequireHud(d->CreateTexture2D(&t,nullptr,&alpha));RequireHud(d->CreateRenderTargetView(alpha.Get(),nullptr,&alphaView));
        RequireHud(d->CreateVertexShader(FullscreenShader,sizeof(FullscreenShader),nullptr,&vertex));
        RequireHud(d->CreatePixelShader(UiAlphaShader,sizeof(UiAlphaShader),nullptr,&pixel));
        RequireHud(d->CreatePixelShader(UiAlphaFlippedShader,sizeof(UiAlphaFlippedShader),nullptr,&flippedPixel));
        D3D11_BLEND_DESC b{};auto& rt=b.RenderTarget[0];rt.BlendEnable=TRUE;
        rt.SrcBlend=rt.SrcBlendAlpha=D3D11_BLEND_ONE;rt.DestBlend=rt.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp=rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALPHA;
        RequireHud(d->CreateBlendState(&b,&opacityBlend));HookDraws(c);
    }
    void Invalidate(const char* message) override {valid=false;reason=message;}
    bool Targets(ID3D11Resource* r)const override{return target.Get()==r;}
    void Replay(const std::function<void()>& draw) override {
        if(!valid)return;
        std::array<ComPtr<ID3D11RenderTargetView>,8> owned;
        ID3D11RenderTargetView* targets[8]{};ComPtr<ID3D11DepthStencilView> depth;
        context->OMGetRenderTargets(8,targets,&depth);for(int i=0;i<8;++i)owned[i].Attach(targets[i]);
        ComPtr<ID3D11Resource> bound;if(targets[0])targets[0]->GetResource(&bound);
        if(bound.Get()!=target.Get()){Invalidate("HUD render target changed");return;}
        for(int i=1;i<8;++i)if(targets[i]){Invalidate("multiple HUD render targets");return;}
        ComPtr<ID3D11BlendState> blend;FLOAT factors[4]{};UINT mask{};context->OMGetBlendState(&blend,factors,&mask);
        D3D11_BLEND_DESC bd{};if(blend)blend->GetDesc(&bd);
        else {bd.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;}
        const auto& rt=bd.RenderTarget[0];
        // Stencil-only mask draws must run only once on the real target.
        if(!(rt.RenderTargetWriteMask&7))return;
        if((rt.RenderTargetWriteMask&7)!=7 || bd.AlphaToCoverageEnable || !rt.BlendEnable || rt.BlendOp!=D3D11_BLEND_OP_ADD ||
           rt.DestBlend!=D3D11_BLEND_INV_SRC_ALPHA || (rt.SrcBlend!=D3D11_BLEND_SRC_ALPHA && rt.SrcBlend!=D3D11_BLEND_ONE)){
            Invalidate("unsupported HUD blend (opaque/additive/custom)");return;
        }
        // Replay may not write stream-output buffers, UAVs, depth or stencil.
        ID3D11UnorderedAccessView* uavs[8]{};context->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,uavs);
        bool sideEffects=false;for(auto u:uavs)if(u){sideEffects=true;u->Release();}
        ID3D11Buffer* outputs[4]{};context->SOGetTargets(4,outputs);for(auto o:outputs)if(o){sideEffects=true;o->Release();}
        ComPtr<ID3D11Predicate> predicate;BOOL predicateValue{};context->GetPredication(&predicate,&predicateValue);
        if(sideEffects || predicate){Invalidate("HUD shader side effects or predication");return;}
        ComPtr<ID3D11DepthStencilState> originalDepth;UINT stencil{};context->OMGetDepthStencilState(&originalDepth,&stencil);
        auto found=depths.find(originalDepth.Get());
        if(found==depths.end()){
            if(depths.size()>=128){Invalidate("HUD depth-state cache limit");return;}
            D3D11_DEPTH_STENCIL_DESC dd{};
            if(originalDepth)originalDepth->GetDesc(&dd);
            else {dd.DepthEnable=TRUE;dd.DepthFunc=D3D11_COMPARISON_LESS;}
            dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;dd.StencilWriteMask=0;
            dd.FrontFace.StencilFailOp=dd.FrontFace.StencilDepthFailOp=dd.FrontFace.StencilPassOp=D3D11_STENCIL_OP_KEEP;
            dd.BackFace.StencilFailOp=dd.BackFace.StencilDepthFailOp=dd.BackFace.StencilPassOp=D3D11_STENCIL_OP_KEEP;
            if(!dd.StencilEnable)dd.FrontFace.StencilFunc=dd.BackFace.StencilFunc=D3D11_COMPARISON_ALWAYS;
            DepthPair pair;pair.original=originalDepth;
            if(FAILED(device->CreateDepthStencilState(&dd,&pair.readOnly))){Invalidate("HUD read-only depth state unavailable");return;}
            found=depths.emplace(originalDepth.Get(),std::move(pair)).first;
        }
        auto capture=colorView.Get();context->OMSetRenderTargets(1,&capture,depth.Get());
        context->OMSetBlendState(opacityBlend.Get(),factors,mask);
        context->OMSetDepthStencilState(found->second.readOnly.Get(),stencil);
        draw(); // Same shader, texture opacity, scissor, geometry and stencil tests.
        context->OMSetDepthStencilState(originalDepth.Get(),stencil);
        context->OMSetBlendState(blend.Get(),factors,mask);
        context->OMSetRenderTargets(8,targets,depth.Get());
    }
    void Extract(){
        ComPtr<ID3DDeviceContextState> previous;context1->SwapDeviceContextState(extraction.Get(),&previous);
        auto r=alphaView.Get();context->OMSetRenderTargets(1,&r,nullptr);
        D3D11_VIEWPORT vp{0,0,static_cast<float>(width),static_cast<float>(height),0,1};context->RSSetViewports(1,&vp);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vertex.Get(),nullptr,0);context->PSSetShader(flipRows?flippedPixel.Get():pixel.Get(),nullptr,0);
        auto s=colorInput.Get();context->PSSetShaderResources(0,1,&s);
        context->Draw(3,0);context1->SwapDeviceContextState(previous.Get(),nullptr);
    }
};
HudOpacity::HudOpacity(ID3D11Device* d,ID3D11DeviceContext* c,UINT w,UINT h):impl(std::make_unique<Impl>(d,c,w,h)){}
HudOpacity::~HudOpacity(){Cancel();}
void HudOpacity::Begin(ID3D11Texture2D* target,int frame,bool flipRows){
    Cancel();if(current){current->Invalidate("overlapping HUD captures");return;}
    impl->target=target;impl->frame=frame;impl->flipRows=flipRows;impl->finished=0;impl->valid=true;impl->reason="capturing";
    FLOAT clear[4]{};impl->context->ClearRenderTargetView(impl->colorView.Get(),clear);
    // SwapDeviceContextState and driver lazy initialization can select another
    // dispatch table. Install on the current table after the HUD target is ready.
    HookDraws(impl->context);current=impl.get();
}
bool HudOpacity::End(int frame){
    if(current!=impl.get() || frame!=impl->frame){Cancel();return false;}
    current=nullptr;impl->finished=frame;
    auto table=*reinterpret_cast<void***>(impl->context);
    if(table[13]!=reinterpret_cast<void*>(&DrawHud<13,UINT,UINT>) || table[12]!=reinterpret_cast<void*>(&DrawHud<12,UINT,UINT,INT>))impl->Invalidate("HUD context dispatch changed during capture");
    if(impl->valid){impl->Extract();impl->reason="valid HUD opacity";}
    impl->target.Reset();return impl->valid;
}
void HudOpacity::Cancel(){if(current==impl.get())current=nullptr;impl->valid=false;impl->finished=0;impl->target.Reset();impl->reason="missing or mismatched HUD completion";}
ID3D11Texture2D* HudOpacity::Texture()const{return impl->valid && impl->finished?impl->alpha.Get():nullptr;}
const char* HudOpacity::Reason()const{return impl->reason;}
