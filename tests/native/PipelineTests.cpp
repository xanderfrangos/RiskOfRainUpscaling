#include "Upscaling/UpscalerContext.h"
#include "Presentation/StreamlinePresentation.h"
#include "FrameGeneration/FrameBridge.h"
#include "Presentation/GraphicsHooks.h"
#include "FrameGeneration/FrameGeneration.h"
#include "FrameGeneration/FrameGenerationSpace.h"
#include "Diagnostics/Profiler.h"
#include "shaders/Fullscreen.h"
#include "shaders/HudFixture.h"
#include <DirectXPackedVector.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <vector>
#include <algorithm>
#include <chrono>

static int width=256, height=144;
// --fsr-upscaler: every context uses FSR, which has no alpha upscaling (temporal outlines fall back).
static bool fsrUpscaler=false;
// Streamline's presentation hooks count each present only when DLSS runs.
static unsigned PresentStep(){return fsrUpscaler?0u:1u;}
static bool DrawUiFixture(ID3D11Device* device,ID3D11DeviceContext* context,IDXGISwapChain* chain,int frame){
    ComPtr<ID3D11Device1> device1;ComPtr<ID3D11DeviceContext1> context1;
    if(FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))) || FAILED(context->QueryInterface(IID_PPV_ARGS(&context1))))return false;
    auto level=device->GetFeatureLevel();ComPtr<ID3DDeviceContextState> state,previous;
    if(FAILED(device1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),nullptr,&state)))return false;
    ComPtr<ID3D11Texture2D> producer;ComPtr<ID3D11RenderTargetView> view;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;rd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
    if(FAILED(chain->GetBuffer(0,IID_PPV_ARGS(&producer))) || FAILED(device->CreateRenderTargetView(producer.Get(),&rd,&view)))return false;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;ComPtr<ID3D11BlendState> blend;
    if(FAILED(device->CreateVertexShader(FullscreenShader,sizeof(FullscreenShader),nullptr,&vs)) || FAILED(device->CreatePixelShader(HudFixtureShader,sizeof(HudFixtureShader),nullptr,&ps)))return false;
    D3D11_BLEND_DESC bd{};auto& rt=bd.RenderTarget[0];rt.BlendEnable=TRUE;rt.SrcBlend=rt.SrcBlendAlpha=D3D11_BLEND_SRC_ALPHA;rt.DestBlend=rt.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp=rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;rt.RenderTargetWriteMask=15;if(FAILED(device->CreateBlendState(&bd,&blend)))return false;
    context1->SwapDeviceContextState(state.Get(),&previous);auto r=view.Get();context->OMSetRenderTargets(1,&r,nullptr);context->OMSetBlendState(blend.Get(),nullptr,0xffffffff);
    D3D11_VIEWPORT vp{0,0,16,12,0,1};context->RSSetViewports(1,&vp);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
    auto start=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_FrameGenerationGetBeforeHudEvent());start(-frame,nullptr);start(frame,nullptr);
    context->Draw(3,0);auto end=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_FrameGenerationGetAfterHudEvent());end(frame,nullptr);
    context1->SwapDeviceContextState(previous.Get(),nullptr);return true;
}
static ComPtr<ID3D11Texture2D> Texture(ID3D11Device* device, DXGI_FORMAT format, UINT bind=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET,
    const void* data=nullptr, UINT pitch=0, int w=0, int h=0) {
    D3D11_TEXTURE2D_DESC d{}; d.Width=w?w:width;d.Height=h?h:height;d.MipLevels=d.ArraySize=1;d.Format=format;d.SampleDesc.Count=1;d.BindFlags=bind;
    ComPtr<ID3D11Texture2D> result; D3D11_SUBRESOURCE_DATA initial{data,pitch,0};
    if(FAILED(device->CreateTexture2D(&d,data?&initial:nullptr,&result)))return nullptr;
    return result;
}
// RGBA16 readback at the texture's own size.
static std::vector<uint16_t> Read(ID3D11Device* device,ID3D11DeviceContext* immediate,ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;if(FAILED(device->CreateTexture2D(&d,nullptr,&staging)))return {};
    immediate->CopyResource(staging.Get(),texture);D3D11_MAPPED_SUBRESOURCE mapped{};
    if(FAILED(immediate->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)))return {};
    std::vector<uint16_t> result(d.Width*d.Height*4);
    for(UINT y=0;y<d.Height;++y)std::memcpy(result.data()+y*d.Width*4,static_cast<unsigned char*>(mapped.pData)+y*mapped.RowPitch,d.Width*8);
    immediate->Unmap(staging.Get(),0);return result;
}
// RGBA8 readback at the texture's own size.
static std::vector<uint8_t> Read8(ID3D11Device* device,ID3D11DeviceContext* immediate,ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;if(FAILED(device->CreateTexture2D(&d,nullptr,&staging)))return {};
    immediate->CopyResource(staging.Get(),texture);D3D11_MAPPED_SUBRESOURCE mapped{};
    if(FAILED(immediate->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)))return {};
    std::vector<uint8_t> result(d.Width*d.Height*4);
    for(UINT y=0;y<d.Height;++y)std::memcpy(result.data()+y*d.Width*4,static_cast<unsigned char*>(mapped.pData)+y*mapped.RowPitch,d.Width*4);
    immediate->Unmap(staging.Get(),0);return result;
}
// Interactable outline mask: a render-sized rectangle of a fading highlight
// (colour and alpha scaled by strength), upscaled with sharpened edges.
static int OutlineMaskFixture(ID3D11Device* device,ID3D11DeviceContext* immediate,void* c,int inW,int inH,int outW,int outH,int frame) {
    const int x0=inW/4,x1=inW*3/4,y0=inH/4,y1=inH*3/4;const uint8_t fill[4]{200,100,0,180};
    std::vector<uint8_t> mask(inW*inH*4,0),zeros(outW*outH*4,0);
    for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x)std::memcpy(&mask[(y*inW+x)*4],fill,4);
    const UINT both=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    auto source=Texture(device,DXGI_FORMAT_R8G8B8A8_TYPELESS,both,mask.data(),inW*4,inW,inH);
    auto target=Texture(device,DXGI_FORMAT_R8G8B8A8_TYPELESS,both,zeros.data(),outW*4,outW,outH);
    auto wrong=Texture(device,DXGI_FORMAT_R16G16B16A16_FLOAT,both,nullptr,0,outW,outH);
    if(!source||!target||!wrong)return 220;
    auto oe=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_UpscalerGetOutlineUpscaleEvent());
    // Unregistered: the event leaves the target alone.
    oe(frame,c);if(Read8(device,immediate,target.Get())!=zeros)return 221;
    if(RiskOfRainUpscaling_UpscalerSetOutlineMasks(c,target.Get(),source.Get())!=-1 || RiskOfRainUpscaling_UpscalerSetOutlineMasks(c,source.Get(),wrong.Get())!=-1 ||
       RiskOfRainUpscaling_UpscalerSetOutlineMasks(c,nullptr,target.Get())!=-1 || RiskOfRainUpscaling_UpscalerSetOutlineMasks(nullptr,source.Get(),target.Get())!=-1)return 222;
    if(RiskOfRainUpscaling_UpscalerSetOutlineMasks(c,source.Get(),target.Get())!=0)return 223;
    oe(frame,c);auto out=Read8(device,immediate,target.Get());
    if(out.size()!=zeros.size())return 224;
    // Flat regions keep the faded highlight exactly; nothing overshoots it.
    auto at=[&](int x,int y){return &out[(y*outW+x)*4];};
    for(int y=0;y<outH;++y)for(int x=0;x<outW;++x){
        float u=(x+.5f)*inW/outW,v=(y+.5f)*inH/outH;auto p=at(x,y);
        bool inside=u>x0+1.5f && u<x1-1.5f && v>y0+1.5f && v<y1-1.5f, outside=u<x0-1.5f || u>x1+1.5f || v<y0-1.5f || v>y1+1.5f;
        for(int ch=0;ch<4;++ch){
            if(inside && std::abs(p[ch]-fill[ch])>1)return 225;
            if(outside && p[ch]>1)return 226;
            if(p[ch]>fill[ch]+1)return 227;}
    }
    // Edges one display pixel wide: far fewer partial-coverage pixels than the
    // bilinear upscale (one render pixel wide), and the edge itself is kept.
    int sharpSoft=0,bilinearSoft=0,edge=0;
    auto ramp=[](float position,int a,int b){return std::min(std::clamp(position-(a-.5f),0.f,1.f),std::clamp((b+.5f)-position,0.f,1.f));};
    for(int y=0;y<outH;++y)for(int x=0;x<outW;++x){
        float u=(x+.5f)*inW/outW,v=(y+.5f)*inH/outH,bilinear=ramp(u,x0,x1)*ramp(v,y0,y1),sharp=at(x,y)[3]/static_cast<float>(fill[3]);
        bilinearSoft+=bilinear>.06f && bilinear<.94f;sharpSoft+=sharp>.06f && sharp<.94f;
        edge+=bilinear>.06f && bilinear<.94f && sharp>.06f;
    }
    if(edge==0 || sharpSoft*2>bilinearSoft){printf("Outline mask soft pixels=%d bilinear=%d edge=%d\n",sharpSoft,bilinearSoft,edge);return 228;}
    printf("PASS: outline mask upscale %dx%d -> %dx%d keeps a fading highlight exact without overshoot; %d partial edge pixels vs %d bilinear.\n",inW,inH,outW,outH,sharpSoft,bilinearSoft);
    return 0;
}
// Full-resolution outlines (reversed-Z): object A exists only in the display-
// sized object depth, object B only in the render-sized raw depth (as for a
// highlighted renderer without a ShadowCaster pass); both are in the mask over a
// farther background. depth is the context's raw depth.
static std::vector<float> ReadFloat(ID3D11Device* device,ID3D11DeviceContext* immediate,ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;if(FAILED(device->CreateTexture2D(&d,nullptr,&staging)))return {};
    immediate->CopyResource(staging.Get(),texture);D3D11_MAPPED_SUBRESOURCE mapped{};
    if(FAILED(immediate->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)))return {};
    const UINT bytes=d.Format==DXGI_FORMAT_R16_TYPELESS?2:4;std::vector<float> result(d.Width*d.Height);
    for(UINT y=0;y<d.Height;++y)for(UINT x=0;x<d.Width;++x){
        auto p=static_cast<unsigned char*>(mapped.pData)+y*mapped.RowPitch+x*bytes;
        if(bytes==4)std::memcpy(&result[y*d.Width+x],p,4);
        else {uint16_t h;std::memcpy(&h,p,2);result[y*d.Width+x]=DirectX::PackedVector::XMConvertHalfToFloat(h);}}
    immediate->Unmap(staging.Get(),0);return result;
}
static int FullResolutionOutlineFixture(ID3D11Device* device,ID3D11DeviceContext* immediate,void* c,ID3D11Texture2D* depth,int inW,int inH,int outW,int outH,int frame) {
    // Linear01 depths of about 0.01 and 0.05 at near 0.1 and far 100 or 1000: the
    // game's Sobel ignores depth nearer than 0.001.
    const float background=.002f,object=.01f;
    // A in display pixels; B in render texels, masked where display pixel centres fall inside it.
    const int ax0=outW/8,ax1=outW*3/8,ay0=outH/4,ay1=outH*3/4, bx0=inW*5/8,bx1=inW*7/8,by0=inH/4,by1=inH*3/4;
    std::vector<float> raw(inW*inH*4,0),objectDepths(outW*outH,0),zeros(outW*outH,0);
    for(int y=0;y<inH;++y)for(int x=0;x<inW;++x)raw[(y*inW+x)*4]=x>=bx0 && x<bx1 && y>=by0 && y<by1?object:background;
    immediate->UpdateSubresource(depth,0,nullptr,raw.data(),inW*16,0);
    auto inA=[&](int x,int y){return x>=ax0 && x<ax1 && y>=ay0 && y<ay1;};
    auto inB=[&](int x,int y){float u=(x+.5f)*inW/outW,v=(y+.5f)*inH/outH;return u>=bx0 && u<bx1 && v>=by0 && v<by1;};
    std::vector<uint8_t> mask(outW*outH*4,0);
    for(int y=0;y<outH;++y)for(int x=0;x<outW;++x){
        if(inA(x,y))objectDepths[y*outW+x]=object;
        if(inA(x,y)||inB(x,y)){uint8_t* p=&mask[(y*outW+x)*4];p[0]=255;p[1]=200;p[3]=255;}}
    std::vector<uint16_t> halfZeros(outW*outH,0);
    const UINT both=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    auto maskTexture=Texture(device,DXGI_FORMAT_R8G8B8A8_TYPELESS,both,mask.data(),outW*4,outW,outH);
    auto objectDepth=Texture(device,DXGI_FORMAT_R32_TYPELESS,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_DEPTH_STENCIL,objectDepths.data(),outW*4,outW,outH);
    auto occlusion=Texture(device,DXGI_FORMAT_R32_TYPELESS,both,zeros.data(),outW*4,outW,outH);
    auto sobel=Texture(device,DXGI_FORMAT_R16_TYPELESS,both,halfZeros.data(),outW*2,outW,outH);
    auto renderSized=Texture(device,DXGI_FORMAT_R32_TYPELESS,both,nullptr,0,inW,inH);
    if(!maskTexture||!objectDepth||!occlusion||!sobel||!renderSized)return 240;
    auto oe=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_UpscalerGetOutlineOcclusionEvent());
    auto se=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_UpscalerGetOutlineSobelEvent());
    // Unregistered: the events leave the targets alone.
    oe(frame,c);se(frame,c);
    if(RiskOfRainUpscaling_UpscalerFullResolutionOutlinesActive(c)!=0 || ReadFloat(device,immediate,occlusion.Get())!=zeros || ReadFloat(device,immediate,sobel.Get())!=zeros)return 241;
    auto set=[&](void* m,void* o,void* d,void* s,int reversed){return RiskOfRainUpscaling_UpscalerSetFullResolutionOutlines(c,m,o,d,s,reversed);};
    if(set(occlusion.Get(),objectDepth.Get(),occlusion.Get(),sobel.Get(),1)!=-1 || set(maskTexture.Get(),occlusion.Get(),occlusion.Get(),sobel.Get(),1)!=-1 ||
       set(maskTexture.Get(),objectDepth.Get(),renderSized.Get(),sobel.Get(),1)!=-1 || set(maskTexture.Get(),objectDepth.Get(),occlusion.Get(),occlusion.Get(),1)!=-1 ||
       set(maskTexture.Get(),objectDepth.Get(),occlusion.Get(),sobel.Get(),2)!=-1 || set(nullptr,objectDepth.Get(),occlusion.Get(),sobel.Get(),1)!=-1 ||
       RiskOfRainUpscaling_UpscalerSetFullResolutionOutlines(nullptr,maskTexture.Get(),objectDepth.Get(),occlusion.Get(),sobel.Get(),1)!=-1)return 242;
    if(set(maskTexture.Get(),objectDepth.Get(),occlusion.Get(),sobel.Get(),1)!=0 || RiskOfRainUpscaling_UpscalerFullResolutionOutlinesActive(c)!=1)return 243;
    oe(frame,c);se(frame,c);
    auto occluded=ReadFloat(device,immediate,occlusion.Get()),encoded=ReadFloat(device,immediate,sobel.Get());
    if(occluded.size()!=zeros.size() || encoded.size()!=zeros.size())return 244;
    auto texel=[&](int x,int y){return raw[(std::clamp(y,0,inH-1)*inW+std::clamp(x,0,inW-1))*4];};
    // The game's composite decodes saturate(sobel*41-1).
    auto weight=[&](int x,int y){return std::clamp(encoded[y*outW+x]*41-1,0.f,1.f);};
    int edges[2]{},interior[2]{};
    for(int y=0;y<outH;++y)for(int x=0;x<outW;++x){
        float u=(x+.5f)*inW/outW-.5f,v=(y+.5f)*inH/outH-.5f;
        int cx=static_cast<int>(std::floor(u+.5f)),cy=static_cast<int>(std::floor(v+.5f));
        // Occlusion: the farthest of the 3x3 render texels, never nearer than the pixel's own.
        float farthest=texel(cx,cy);
        for(int j=-1;j<=1;++j)for(int i=-1;i<=1;++i)farthest=std::min(farthest,texel(cx+i,cy+j));
        float o=occluded[y*outW+x];
        if(std::abs(o-farthest)>1e-6f || o>texel(cx,cy)+1e-6f){printf("Occlusion depth at %d,%d=%.6f expected %.6f\n",x,y,o,farthest);return 245;}
        bool a=inA(x,y),b=inB(x,y);
        float w=weight(x,y);
        if(!a && !b){if(encoded[y*outW+x]!=0){printf("Sobel outside the mask at %d,%d=%.4f\n",x,y,encoded[y*outW+x]);return 246;}continue;}
        auto inside=[&](int px,int py){return a?inA(px,py):inB(px,py);};
        int k=a?0:1;
        // Expected: the share of the eight sub-pixel positions with a Sobel tap
        // (0.002*(size+1) pixels away) outside the object, where depth is farther.
        const float samples[8][2]{{1,-3},{-1,3},{5,1},{-3,-5},{-5,5},{-7,-1},{3,7},{7,-7}};
        const float ox=.002f*(outW+1),oy=.002f*(outH+1);int hits=0;
        for(auto& sp:samples){float px=x+.5f+sp[0]/16,py=y+.5f+sp[1]/16;bool farther=false;
            for(int j=-1;j<=1;++j)for(int i=-1;i<=1;++i)
                if(!inside(static_cast<int>(std::floor(px+i*ox)),static_cast<int>(std::floor(py+j*oy))))farther=true;
            hits+=farther;}
        float expected=hits/8.f;
        if(std::abs(w-expected)>.01f){printf("Sobel %c at %d,%d=%.4f expected %.4f\n",a?'A':'B',x,y,w,expected);return 247;}
        if(hits==0)++interior[k]; else if(hits<8)++edges[k];
    }
    if(!edges[0]||!edges[1]||!interior[0]||!interior[1])return 249;
    printf("PASS: full-resolution outlines %dx%d -> %dx%d: conservative occlusion depth; Sobel edges from the object depth (%d anti-aliased edge pixels) and from render depth without it (%d), each the exact 8-sample coverage, none inside or outside the mask.\n",inW,inH,outW,outH,edges[0],edges[1]);
    return 0;
}
static CameraFrame Frame(int index) {
    CameraFrame f{};f.size=sizeof(f);f.version=1;f.frame=index;f.reset=index==1?1:0;
    // Row-vector, right-handed D3D projection, near .1, far 100.
    float aspect=static_cast<float>(width)/height;float scale=1/std::tan(0.5f);
    f.projection[0]=scale/aspect;f.projection[5]=scale;f.projection[10]=-100.f/99.9f;f.projection[11]=-1;f.projection[14]=-10.f/99.9f;
    f.inverseProjection[0]=aspect/scale;f.inverseProjection[5]=1/scale;f.inverseProjection[11]=-99.9f/10;f.inverseProjection[14]=-1;f.inverseProjection[15]=10;
    for(int i=0;i<4;++i){f.clipToPrevious[5*i]=1;f.previousToClip[5*i]=1;}
    f.motionScaleX=f.motionScaleY=1;f.nearPlane=.1f;f.farPlane=100;f.fov=1;f.aspect=aspect;
    f.up[1]=1;f.right[0]=1;f.forward[2]=-1;return f;
}
// FG constants must describe the top-down swap chain, not Unity's flipped RT space.
static bool FrameGenerationSpaceCheck() {
    auto f=Frame(2);f.jitterX=.125f;f.jitterY=.25f;
    for(int i=0;i<16;++i){f.clipToPrevious[i]=static_cast<float>(i+1);f.previousToClip[i]=static_cast<float>(17-i);}
    auto g=FrameGenerationPacket(f);
    auto sign=[](int i){return i==1?-1.f:1.f;};
    for(int r=0;r<4;++r)for(int c=0;c<4;++c){int i=r*4+c;
        if(g.projection[i]!=f.projection[i]*sign(c) || g.inverseProjection[i]!=f.inverseProjection[i]*sign(r) ||
           g.clipToPrevious[i]!=f.clipToPrevious[i]*sign(r)*sign(c) || g.previousToClip[i]!=f.previousToClip[i]*sign(r)*sign(c))return false;}
    // A view-space point lands at the mirrored clip Y, and the inverse undoes it.
    float view[4]{.3f,.2f,-5,1},clip[4]{},flipped[4]{},back[4]{};
    for(int c=0;c<4;++c)for(int k=0;k<4;++k){clip[c]+=view[k]*f.projection[k*4+c];flipped[c]+=view[k]*g.projection[k*4+c];}
    for(int c=0;c<4;++c)for(int k=0;k<4;++k)back[c]+=flipped[k]*g.inverseProjection[k*4+c];
    for(int c=0;c<4;++c)if(flipped[c]!=clip[c]*sign(c) || std::abs(back[c]/back[3]-view[c])>1e-4f)return false;
    return g.jitterX==f.jitterX && g.jitterY==-f.jitterY && g.frame==f.frame && g.motionScaleY==f.motionScaleY;
}
// Lower-resolution DLSS: render-sized color/depth/motion, display-sized output.
// WARP proves the bilinear fallback; hardware proves real Quality-mode evaluation.
// With fg, the bridge must tag render-sized depth/motion beside display-sized color.
static int UpscaleFixture(ID3D11Device* device,ID3D11DeviceContext* immediate,bool hardware,IDXGISwapChain* chain,bool fg=false,int firstFrame=1) {
    using namespace DirectX::PackedVector;
    const int outW=width,outH=height,inW=(outW*2+1)/3,inH=(outH*2+1)/3;
    std::vector<uint16_t> pattern(inW*inH*4);double patternMean=0;
    for(int y=0;y<inH;++y)for(int x=0;x<inW;++x){auto i=(y*inW+x)*4;float r=(x%13)<6?.8f:.1f,g=(y%11)<5?.6f:.15f;
        pattern[i]=XMConvertFloatToHalf(r);pattern[i+1]=XMConvertFloatToHalf(g);pattern[i+2]=XMConvertFloatToHalf(.3f);pattern[i+3]=XMConvertFloatToHalf(1);patternMean+=r+g+.3f;}
    patternMean/=3.0*inW*inH;
    const UINT both=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    auto color=Texture(device,DXGI_FORMAT_R16G16B16A16_TYPELESS,both,pattern.data(),inW*8,inW,inH);
    auto output=Texture(device,DXGI_FORMAT_R16G16B16A16_TYPELESS,both,nullptr,0,outW,outH);
    auto depth=Texture(device,DXGI_FORMAT_R32G32B32A32_TYPELESS,both,nullptr,0,inW,inH),motion=Texture(device,DXGI_FORMAT_R32G32B32A32_TYPELESS,both,nullptr,0,inW,inH);
    // A row gradient proves FG receives depth flipped into swap-chain row order.
    std::vector<float> depths(inW*inH);
    for(int y=0;y<inH;++y)for(int x=0;x<inW;++x)depths[y*inW+x]=.25f+.5f*static_cast<float>(y)/inH;
    auto sceneDepth=Texture(device,DXGI_FORMAT_R32_TYPELESS,D3D11_BIND_DEPTH_STENCIL,depths.data(),inW*4,inW,inH);
    if(!color||!output||!depth||!motion||!sceneDepth)return 100;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
    ComPtr<ID3D11ShaderResourceView> motionSRV;D3D11_RENDER_TARGET_VIEW_DESC rd{};rd.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;rd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> rtv,motionRTV;D3D11_RENDER_TARGET_VIEW_DESC md{};md.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;md.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_DEPTH_STENCIL_VIEW_DESC ds{};ds.Format=DXGI_FORMAT_D32_FLOAT;ds.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;ComPtr<ID3D11DepthStencilView> dsv;
    if(FAILED(device->CreateShaderResourceView(motion.Get(),&sd,&motionSRV)) || FAILED(device->CreateRenderTargetView(color.Get(),&rd,&rtv)) ||
       FAILED(device->CreateRenderTargetView(motion.Get(),&md,&motionRTV)) || FAILED(device->CreateDepthStencilView(sceneDepth.Get(),&ds,&dsv)))return 101;
    float zero[4]{};immediate->ClearRenderTargetView(motionRTV.Get(),zero);
    SceneInputsDescription b{sizeof(b),4,inW,inH,depth.Get()};
    auto bc=RiskOfRainUpscaling_SceneInputsCreate(&b);if(!bc)return 102;
    UpscalerDescription d{sizeof(d),2,inW,inH,bc,color.Get(),depth.Get(),motion.Get(),output.Get(),outW,outH,1,0};
    auto invalid=d;invalid.quality=0;if(RiskOfRainUpscaling_UpscalerCreate(&invalid))return 103;   // DLAA must be 100%.
    invalid=d;invalid.quality=5;if(RiskOfRainUpscaling_UpscalerCreate(&invalid))return 104;
    invalid=d;invalid.outputWidth=inW-1;if(RiskOfRainUpscaling_UpscalerCreate(&invalid))return 105; // Never downscale.
    invalid=d;invalid.output=color.Get();if(RiskOfRainUpscaling_UpscalerCreate(&invalid))return 106;
    invalid=d;invalid.placement=2;if(RiskOfRainUpscaling_UpscalerCreate(&invalid))return 127; // Unknown placement.
    auto c=RiskOfRainUpscaling_UpscalerCreate(&d);if(!c)return 107;
    auto ce=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_UpscalerGetEvaluateEvent());
    auto be=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_SceneInputsGetEvent());
    auto bf=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_SceneInputsGetFrameEvent());
    auto target=rtv.Get();immediate->OMSetRenderTargets(1,&target,dsv.Get());
    auto f=Frame(firstFrame);if(RiskOfRainUpscaling_UpscalerSubmitFrame(c,&f))return 108;
    ce(-2,c);
    int frames=fg?12:hardware?8:1;
    if(hardware && RiskOfRainUpscaling_UpscalerState(c)!=1)return 109;
    if(!hardware && RiskOfRainUpscaling_UpscalerState(c)!=-2)return 110;
    auto fe=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_FrameGenerationGetBeforeHudEvent());
    for(int i=firstFrame;i<firstFrame+frames;++i){
        if(fg){RiskOfRainUpscaling_FrameGenerationMarker(0,i);RiskOfRainUpscaling_FrameGenerationMarker(1,i);RiskOfRainUpscaling_FrameGenerationMarker(2,i);fe(-i,nullptr);}
        f=Frame(i);if(i>firstFrame && RiskOfRainUpscaling_UpscalerSubmitFrame(c,&f))return 111;
        bf(i,bc);immediate->OMSetRenderTargets(1,&target,dsv.Get());be(90,bc);be(100,bc);
        auto srv=motionSRV.Get();immediate->PSSetShaderResources(0,1,&srv);be(102,bc);
        ce(i,c);if(RiskOfRainUpscaling_UpscalerState(c)!=(hardware?2:-2))return 112;
        if(fg){
            // Synthetic stand-in for Unity's final blit, then the pre-HUD FG event.
            auto out=Read(device,immediate,output.Get());std::vector<unsigned char> converted(out.size());
            for(size_t p=0;p<out.size();++p)converted[p]=static_cast<unsigned char>(std::clamp(XMConvertHalfToFloat(out[p]),0.f,1.f)*255.f+.5f);
            ComPtr<ID3D11Texture2D> producer;if(FAILED(chain->GetBuffer(0,IID_PPV_ARGS(&producer))))return 120;
            immediate->UpdateSubresource(producer.Get(),0,nullptr,converted.data(),outW*4,0);producer.Reset();
            fe(i,nullptr);
        }
        if(chain && FAILED(chain->Present(0,0)))return 113;
    }
    if(fg){
        if(RiskOfRainUpscaling_FrameGenerationState()!=3){printf("Lower-resolution FG state=%d\n",RiskOfRainUpscaling_FrameGenerationState());return 121;}
        std::vector<float> readDepth(inW*inH);
        if(FAILED(FrameBridgeInputReadback(chain,1,readDepth.data(),static_cast<unsigned>(readDepth.size()*4))))return 122;
        for(int y=0;y<inH;++y)for(int x=0;x<inW;++x)if(readDepth[y*inW+x]!=depths[(inH-1-y)*inW+x])return 123;
        std::vector<uint16_t> readMotion(inW*inH*2);
        if(FAILED(FrameBridgeInputReadback(chain,2,readMotion.data(),static_cast<unsigned>(readMotion.size()*2))))return 124;
        for(auto m:readMotion)if(XMConvertHalfToFloat(m)!=0.f)return 125;
        std::vector<unsigned char> readColor(outW*outH*4);
        if(FAILED(FrameBridgeInputReadback(chain,3,readColor.data(),static_cast<unsigned>(readColor.size()))))return 126;
    }
    ComPtr<ID3D11RenderTargetView> restored;immediate->OMGetRenderTargets(1,&restored,nullptr);
    if(restored.Get()!=rtv.Get())return 114;
    auto pixels=Read(device,immediate,output.Get());if(pixels.size()!=static_cast<size_t>(outW*outH*4))return 115;
    double mean=0;
    for(size_t i=0;i<pixels.size();++i){float v=XMConvertHalfToFloat(pixels[i]);if(!std::isfinite(v))return 116;if(i%4!=3)mean+=v;else if(std::abs(v-1)>.01f)return 117;}
    mean/=3.0*outW*outH;
    // Bilinear and DLSS both preserve the image's average brightness closely.
    if(std::abs(mean-patternMean)>.05)return 118;
    ce(-1,c);if(RiskOfRainUpscaling_UpscalerState(c)!=-1)return 119;RiskOfRainUpscaling_UpscalerDestroy(c);be(199,bc);RiskOfRainUpscaling_SceneInputsDestroy(bc);
    printf("PASS: lower-resolution %s %dx%d -> %dx%d (mean %.4f vs %.4f), render/display ABI checks, opaque alpha and binding restoration.\n",
        fg?"DLSS Quality + FG with render-sized depth/motion tags":hardware?"DLSS Quality evaluation":"bilinear fallback",inW,inH,outW,outH,mean,patternMean);
    return 0;
}
// Temporal outlines: a highlighted rectangle whose border has Sobel edges.
struct OutlineInputs {
    int inW,inH,outW,outH,x0,x1,y0,y1;
    std::vector<uint16_t> grey;
    ComPtr<ID3D11Texture2D> mask,sobel,composite;
    OutlineInputs(ID3D11Device* device,int renderWidth,int renderHeight,int displayWidth,int displayHeight)
        :inW(renderWidth),inH(renderHeight),outW(displayWidth),outH(displayHeight),x0(inW/4),x1(inW*3/4),y0(inH/4),y1(inH*3/4),grey(outW*outH*4) {
        using namespace DirectX::PackedVector;
        std::vector<uint8_t> highlight(inW*inH*4,0),edges(inW*inH,0);
        for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x){
            uint8_t* p=&highlight[(y*inW+x)*4];p[0]=255;p[1]=204;p[2]=0;p[3]=255;
            if(x==x0||x==x1-1||y==y0||y==y1-1)edges[y*inW+x]=255;}
        for(size_t i=0;i<grey.size();++i)grey[i]=XMConvertFloatToHalf(i%4==3?.5f:.2f);
        const UINT both=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        mask=Texture(device,DXGI_FORMAT_R8G8B8A8_TYPELESS,both,highlight.data(),inW*4,inW,inH);
        sobel=Texture(device,DXGI_FORMAT_R8_TYPELESS,both,edges.data(),inW,inW,inH);
        composite=Texture(device,DXGI_FORMAT_R16G16B16A16_TYPELESS,both,grey.data(),outW*8,outW,outH);
    }
    // Swapped, wrongly sized or wrongly formatted textures are rejected.
    int Register(void* c,ID3D11Texture2D* renderSized) {
        if(!mask||!sobel||!composite)return 230;
        if(RiskOfRainUpscaling_UpscalerSetTemporalOutlines(c,composite.Get(),sobel.Get(),composite.Get())!=-1 || RiskOfRainUpscaling_UpscalerSetTemporalOutlines(c,mask.Get(),mask.Get(),composite.Get())!=-1 ||
           (outW!=inW && RiskOfRainUpscaling_UpscalerSetTemporalOutlines(c,mask.Get(),sobel.Get(),renderSized)!=-1))return 231;
        return RiskOfRainUpscaling_UpscalerSetTemporalOutlines(c,mask.Get(),sobel.Get(),composite.Get())==0?0:232;
    }
    // Composites over grey and measures the outline weight from red: the border
    // line (peak), the tinted interior and the outside (largest error from the
    // game's weights 0.0275 and 0). Alpha must be untouched.
    bool Weights(ID3D11Device* device,ID3D11DeviceContext* immediate,void* c,int frame,float& border,float& interior,float& outside) {
        using DirectX::PackedVector::XMConvertHalfToFloat;
        immediate->UpdateSubresource(composite.Get(),0,nullptr,grey.data(),outW*8,0);
        reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_UpscalerGetOutlineCompositeEvent())(frame,c);
        auto pixels=Read(device,immediate,composite.Get());border=interior=outside=0;
        if(pixels.size()!=grey.size())return false;
        for(int y=0;y<outH;++y)for(int x=0;x<outW;++x){
            auto p=&pixels[(y*outW+x)*4];if(XMConvertHalfToFloat(p[3])!=.5f)return false;
            float w=(XMConvertHalfToFloat(p[0])-.2f)/.8f,u=(x+.5f)*inW/outW,v=(y+.5f)*inH/outH;
            float inset=std::min(std::min(u-x0,x1-u),std::min(v-y0,y1-v));
            if(inset>2.5f)interior=std::max(interior,std::abs(w-.0275f));
            else if(inset<-2)outside=std::max(outside,std::abs(w));
            else if(inset>0 && inset<1)border=std::max(border,w);
        }
        return true;
    }
    // The reconstructed weight after evaluation, then the current render-sized
    // weight for a frame DLSS did not evaluate.
    int Check(ID3D11Device* device,ID3D11DeviceContext* immediate,void* c,int evaluatedFrame,float& reconstructed) {
        float border=0,interior=0,outside=0;
        if(!Weights(device,immediate,c,evaluatedFrame,border,interior,outside) || border<.6f || interior>.03f || outside>.02f){
            printf("Temporal outline border=%.3f interior=%.3f outside=%.3f\n",border,interior,outside);return 234;}
        reconstructed=border;
        if(!Weights(device,immediate,c,evaluatedFrame+50,border,interior,outside) || border<.99f || interior>.002f || outside>.002f){
            printf("Fallback outline border=%.3f interior=%.3f outside=%.3f\n",border,interior,outside);return 235;}
        return 0;
    }
};
// DLAA before post-processing: the HDR scene target bound after transparents is
// evaluated in place; RCAS later runs from color to output through its own event.
static int PrePostFixture(ID3D11Device* device,ID3D11DeviceContext* immediate,bool hardware,IDXGISwapChain* chain,int firstFrame) {
    using namespace DirectX::PackedVector;
    const uint16_t nan=0x7e00,inf=0x7c00;
    std::vector<uint16_t> scene(width*height*4);double sanitizedMean=0;
    for(int y=0;y<height;++y)for(int x=0;x<width;++x){auto i=(y*width+x)*4;float r=(x%13)<6?8.f:.1f;
        scene[i]=XMConvertFloatToHalf(r);scene[i+1]=XMConvertFloatToHalf((y%11)<5?.6f:.15f);scene[i+2]=XMConvertFloatToHalf(.3f);scene[i+3]=XMConvertFloatToHalf(static_cast<float>((x+y)%7)/6);}
    // Unity's post-stack NaN filter runs after this seam; DLSS must never see these.
    scene[(5*width+5)*4]=nan;scene[(5*width+6)*4+1]=inf;
    for(size_t i=0;i<scene.size();++i)if(i%4==0){float v=XMConvertHalfToFloat(scene[i]);sanitizedMean+=std::isfinite(v)?v:0;}
    sanitizedMean/=static_cast<double>(width)*height;
    const UINT both=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    std::vector<uint16_t> zeros(width*height*4,0);
    auto color=Texture(device,DXGI_FORMAT_R16G16B16A16_TYPELESS,both,scene.data(),width*8);
    auto output=Texture(device,DXGI_FORMAT_R16G16B16A16_TYPELESS,both,zeros.data(),width*8);
    auto depth=Texture(device,DXGI_FORMAT_R32G32B32A32_TYPELESS),motion=Texture(device,DXGI_FORMAT_R32G32B32A32_TYPELESS);
    std::vector<float> depths(width*height,.5f);
    auto sceneDepth=Texture(device,DXGI_FORMAT_R32_TYPELESS,D3D11_BIND_DEPTH_STENCIL,depths.data(),width*4);
    if(!color||!output||!depth||!motion||!sceneDepth)return 150;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};rd.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;rd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_RENDER_TARGET_VIEW_DESC md{};md.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;md.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_DEPTH_STENCIL_VIEW_DESC ds{};ds.Format=DXGI_FORMAT_D32_FLOAT;ds.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11ShaderResourceView> motionSRV;ComPtr<ID3D11RenderTargetView> rtv,motionRTV;ComPtr<ID3D11DepthStencilView> dsv;
    if(FAILED(device->CreateShaderResourceView(motion.Get(),&sd,&motionSRV)) || FAILED(device->CreateRenderTargetView(color.Get(),&rd,&rtv)) ||
       FAILED(device->CreateRenderTargetView(motion.Get(),&md,&motionRTV)) || FAILED(device->CreateDepthStencilView(sceneDepth.Get(),&ds,&dsv)))return 151;
    float zero[4]{};immediate->ClearRenderTargetView(motionRTV.Get(),zero);
    SceneInputsDescription b{sizeof(b),4,width,height,depth.Get()};
    auto bc=RiskOfRainUpscaling_SceneInputsCreate(&b);if(!bc)return 152;
    UpscalerDescription d{sizeof(d),2,width,height,bc,color.Get(),depth.Get(),motion.Get(),output.Get(),width,height,0,1};
    auto invalid=d;invalid.placement=2;if(RiskOfRainUpscaling_UpscalerCreate(&invalid))return 153;
    invalid.placement=-1;if(RiskOfRainUpscaling_UpscalerCreate(&invalid))return 153;
    auto c=RiskOfRainUpscaling_UpscalerCreate(&d);if(!c)return 154;
    auto ce=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_UpscalerGetEvaluateEvent());
    auto se=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_UpscalerGetSharpenEvent());
    auto be=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_SceneInputsGetEvent());
    auto bf=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_SceneInputsGetFrameEvent());
    auto target=rtv.Get();immediate->OMSetRenderTargets(1,&target,dsv.Get());
    D3D11_VIEWPORT viewport{3,4,5,6,0,1};immediate->RSSetViewports(1,&viewport);
    OutlineInputs outlines(device,width,height,width,height);
    if(int failed=outlines.Register(c,nullptr))return failed;
    ce(-2,c);
    if(RiskOfRainUpscaling_UpscalerState(c)!=(hardware?1:-2))return 155;
    if(RiskOfRainUpscaling_UpscalerTemporalOutlinesActive(c)!=(hardware && !fsrUpscaler?1:0)){printf("Temporal outline state=%d\n",RiskOfRainUpscaling_UpscalerTemporalOutlinesActive(c));return 233;}
    const int frames=hardware?32:1;
    for(int i=firstFrame;i<firstFrame+frames;++i){
        // A fresh scene each frame; the evaluation overwrote the last one.
        immediate->UpdateSubresource(color.Get(),0,nullptr,scene.data(),width*8,0);
        auto f=Frame(i);if(i==firstFrame)f.reset=1;
        if(RiskOfRainUpscaling_UpscalerSubmitFrame(c,&f)!=(hardware?0:-1))return 156; // Failed contexts reject packets.
        bf(i,bc);immediate->OMSetRenderTargets(1,&target,dsv.Get());be(90,bc);be(100,bc);
        auto srv=motionSRV.Get();immediate->PSSetShaderResources(0,1,&srv);be(102,bc);
        ce(i,c);if(RiskOfRainUpscaling_UpscalerState(c)!=(hardware?2:-2))return 157;
        ComPtr<ID3D11RenderTargetView> restored;immediate->OMGetRenderTargets(1,&restored,nullptr);
        D3D11_VIEWPORT vp{};UINT count=1;immediate->RSGetViewports(&count,&vp);
        if(restored.Get()!=rtv.Get() || count!=1 || vp.TopLeftX!=3 || vp.Width!=5)return 158;
        if(chain && FAILED(chain->Present(0,0)))return 159;
    }
    auto evaluated=Read(device,immediate,color.Get());if(evaluated.size()!=scene.size())return 160;
    if(Read(device,immediate,output.Get())!=zeros)return 161; // Evaluation never writes the tail output.
    double mean=0,brightInterior=0;float peak=0;
    using DirectX::PackedVector::XMConvertHalfToFloat;
    if(!hardware){if(evaluated!=scene)return 162;} // Unsupported runtime: scene left bit-exact.
    else {
        for(size_t i=0;i<evaluated.size();++i){
            float v=XMConvertHalfToFloat(evaluated[i]);if(!std::isfinite(v))return 163;
            if(i%4==3){if(evaluated[i]!=scene[i])return 164;}else if(i%4==0){mean+=v;peak=std::max(peak,v);}
        }
        mean/=static_cast<double>(width)*height;
        // HDR highlights survive (no LDR clamp) and DLAA keeps average brightness.
        // This unjittered static fixture cannot converge: both placements soften
        // stripe interiors alike (0.8/0.1 input: SDR 0.64/0.15, HDR 0.66/0.21), and
        // HDR mode resolves 80:1 edges perceptually, lowering the linear mean. Only
        // require that HDR radiance survives well above an LDR clamp.
        double bright=0,dark=0;int samples=0;
        for(int y=20;y<height-20;++y)for(int x=13;x+13<width;x+=13){
            bright+=XMConvertHalfToFloat(evaluated[(y*width+x+2)*4]);dark+=XMConvertHalfToFloat(evaluated[(y*width+x+9)*4]);++samples;}
        bright/=samples;dark/=samples;brightInterior=bright;
        if(peak<4 || bright<4 || dark>1){
            printf("Pre-post DLAA peak=%.4f interiors bright=%.4f dark=%.4f mean=%.4f input=%.4f\n",peak,bright,dark,mean,sanitizedMean);return 165;}
    }
    float outlineBorder=0;
    if(!fsrUpscaler)if(int failed=outlines.Check(device,immediate,c,firstFrame+frames-1,outlineBorder))return failed;
    // Tail RCAS: color now holds tonemapped SDR; 0 is an exact copy, >0 keeps alpha.
    std::vector<uint16_t> sdr(scene.size());
    for(size_t i=0;i<sdr.size();++i)sdr[i]=i%4==3?scene[i]:XMConvertFloatToHalf(((i/4)%13)<6?.8f:.1f);
    immediate->UpdateSubresource(color.Get(),0,nullptr,sdr.data(),width*8,0);
    if(RiskOfRainUpscaling_SetSharpness(0))return 166;
    se(firstFrame+frames,c);if(Read(device,immediate,output.Get())!=sdr)return 167;
    if(RiskOfRainUpscaling_SetSharpness(.7f))return 166;
    se(firstFrame+frames+1,c);auto sharpened=Read(device,immediate,output.Get());
    if(sharpened.size()!=sdr.size() || sharpened==sdr)return 168;
    for(size_t i=0;i<sharpened.size();++i){if(!std::isfinite(XMConvertHalfToFloat(sharpened[i])))return 169;if(i%4==3 && sharpened[i]!=sdr[i])return 170;}
    if(hardware){
        // Missing camera packet: latch Off and leave the scene untouched.
        immediate->UpdateSubresource(color.Get(),0,nullptr,scene.data(),width*8,0);
        int frame=firstFrame+frames+2;bf(frame,bc);immediate->OMSetRenderTargets(1,&target,dsv.Get());be(90,bc);be(100,bc);
        auto srv=motionSRV.Get();immediate->PSSetShaderResources(0,1,&srv);be(102,bc);
        ce(frame,c);if(RiskOfRainUpscaling_UpscalerState(c)!=-2 || Read(device,immediate,color.Get())!=scene)return 171;
        if(chain && FAILED(chain->Present(0,0)))return 159;
    }
    ce(-1,c);if(RiskOfRainUpscaling_UpscalerState(c)!=-1)return 172;RiskOfRainUpscaling_UpscalerDestroy(c);be(199,bc);RiskOfRainUpscaling_SceneInputsDestroy(bc);
    if(hardware)printf("PASS: %d HDR DLAA evaluations before post-processing in place (peak %.2f, bright interior %.2f), NaN/Inf sanitized, alpha/bindings preserved, temporal outline weight through DLAA alpha (border %.2f), tail RCAS event and safe failure.\n",frames,peak,brightInterior,outlineBorder);
    else printf("PASS: before-post-processing DLAA leaves the scene bit-exact without a supported runtime; tail RCAS event copies/sharpens; placement ABI validated.\n");
    return 0;
}
// Lower-resolution DLSS before post-processing: the render-sized HDR scene is
// read, never written; DLSS (or the bilinear fallback) fills the display-sized
// output that Unity's post stack then reads at display size.
static int PrePostUpscaleFixture(ID3D11Device* device,ID3D11DeviceContext* immediate,bool hardware,IDXGISwapChain* chain,int firstFrame) {
    using namespace DirectX::PackedVector;
    const int outW=width,outH=height,inW=(outW*2+1)/3,inH=(outH*2+1)/3;
    const uint16_t nan=0x7e00,inf=0x7c00;
    std::vector<uint16_t> scene(inW*inH*4);
    for(int y=0;y<inH;++y)for(int x=0;x<inW;++x){auto i=(y*inW+x)*4;
        scene[i]=XMConvertFloatToHalf((x%13)<6?8.f:.1f);scene[i+1]=XMConvertFloatToHalf((y%11)<5?.6f:.15f);
        scene[i+2]=XMConvertFloatToHalf(.3f);scene[i+3]=XMConvertFloatToHalf(static_cast<float>((x+y)%7)/6);}
    scene[(5*inW+5)*4]=nan;scene[(5*inW+6)*4+1]=inf;
    const UINT both=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    std::vector<uint16_t> zeros(outW*outH*4,0);
    auto color=Texture(device,DXGI_FORMAT_R16G16B16A16_TYPELESS,both,scene.data(),inW*8,inW,inH);
    auto output=Texture(device,DXGI_FORMAT_R16G16B16A16_TYPELESS,both,zeros.data(),outW*8,outW,outH);
    auto depth=Texture(device,DXGI_FORMAT_R32G32B32A32_TYPELESS,both,nullptr,0,inW,inH),motion=Texture(device,DXGI_FORMAT_R32G32B32A32_TYPELESS,both,nullptr,0,inW,inH);
    std::vector<float> depths(inW*inH,.5f);
    auto sceneDepth=Texture(device,DXGI_FORMAT_R32_TYPELESS,D3D11_BIND_DEPTH_STENCIL,depths.data(),inW*4,inW,inH);
    if(!color||!output||!depth||!motion||!sceneDepth)return 180;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};rd.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;rd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_RENDER_TARGET_VIEW_DESC md{};md.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;md.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_DEPTH_STENCIL_VIEW_DESC ds{};ds.Format=DXGI_FORMAT_D32_FLOAT;ds.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11ShaderResourceView> motionSRV;ComPtr<ID3D11RenderTargetView> rtv,motionRTV;ComPtr<ID3D11DepthStencilView> dsv;
    if(FAILED(device->CreateShaderResourceView(motion.Get(),&sd,&motionSRV)) || FAILED(device->CreateRenderTargetView(color.Get(),&rd,&rtv)) ||
       FAILED(device->CreateRenderTargetView(motion.Get(),&md,&motionRTV)) || FAILED(device->CreateDepthStencilView(sceneDepth.Get(),&ds,&dsv)))return 181;
    float zero[4]{};immediate->ClearRenderTargetView(motionRTV.Get(),zero);
    SceneInputsDescription b{sizeof(b),4,inW,inH,depth.Get()};
    auto bc=RiskOfRainUpscaling_SceneInputsCreate(&b);if(!bc)return 182;
    UpscalerDescription d{sizeof(d),2,inW,inH,bc,color.Get(),depth.Get(),motion.Get(),output.Get(),outW,outH,1,1};
    auto c=RiskOfRainUpscaling_UpscalerCreate(&d);if(!c)return 183;
    auto ce=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_UpscalerGetEvaluateEvent());
    auto se=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_UpscalerGetSharpenEvent());
    auto fe=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_UpscalerGetFallbackEvent());
    auto be=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_SceneInputsGetEvent());
    auto bf=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_SceneInputsGetFrameEvent());
    auto target=rtv.Get();
    // Stand-in for Unity's frame up to our after-transparents event.
    auto stages=[&](int frame){
        bf(frame,bc);immediate->OMSetRenderTargets(1,&target,dsv.Get());be(90,bc);be(100,bc);
        auto srv=motionSRV.Get();immediate->PSSetShaderResources(0,1,&srv);be(102,bc);};
    // Display-sized HDR output: finite, highlights well above an LDR clamp.
    auto hdrOutput=[&](float& peak){
        auto pixels=Read(device,immediate,output.Get());peak=0;
        if(pixels.size()!=zeros.size())return false;
        for(size_t i=0;i<pixels.size();++i){float v=XMConvertHalfToFloat(pixels[i]);if(!std::isfinite(v))return false;if(i%4==0)peak=std::max(peak,v);}
        return peak>=4;};
    D3D11_VIEWPORT viewport{3,4,5,6,0,1};
    OutlineInputs outlines(device,inW,inH,outW,outH);
    if(int failed=outlines.Register(c,color.Get()))return failed;
    ce(-2,c);
    if(RiskOfRainUpscaling_UpscalerState(c)!=(hardware?1:-2))return 184;
    if(RiskOfRainUpscaling_UpscalerTemporalOutlinesActive(c)!=(hardware && !fsrUpscaler?1:0)){printf("Temporal outline state=%d\n",RiskOfRainUpscaling_UpscalerTemporalOutlinesActive(c));return 233;}
    const int frames=hardware?16:1;float peak=0;
    for(int i=firstFrame;i<firstFrame+frames;++i){
        auto f=Frame(i);if(i==firstFrame)f.reset=1;
        if(RiskOfRainUpscaling_UpscalerSubmitFrame(c,&f)!=(hardware?0:-1))return 185;
        stages(i);immediate->RSSetViewports(1,&viewport);
        ce(i,c);if(RiskOfRainUpscaling_UpscalerState(c)!=(hardware?2:-2))return 186;
        ComPtr<ID3D11RenderTargetView> restored;immediate->OMGetRenderTargets(1,&restored,nullptr);
        D3D11_VIEWPORT vp{};UINT count=1;immediate->RSGetViewports(&count,&vp);
        if(restored.Get()!=rtv.Get() || count!=1 || vp.TopLeftX!=3 || vp.Width!=5)return 187;
        if(chain && FAILED(chain->Present(0,0)))return 188;
    }
    // The scene is an input only; the post stack reads the output.
    if(Read(device,immediate,color.Get())!=scene)return 189;
    if(!hdrOutput(peak)){printf("Pre-post upscale peak=%.4f\n",peak);return 190;}
    if(hardware){
        // DLSS output keeps the nearest render pixel's original alpha.
        auto pixels=Read(device,immediate,output.Get());
        for(int y=0;y<outH;++y)for(int x=0;x<outW;++x)
            if(pixels[(y*outW+x)*4+3]!=scene[((y*inH/outH)*inW+x*inW/outW)*4+3])return 191;
    }
    float reconstructedBorder=0;
    if(!fsrUpscaler)if(int failed=outlines.Check(device,immediate,c,firstFrame+frames-1,reconstructedBorder))return failed;
    const float evaluatedPeak=peak;
    // Unprepared frame: bilinear, sanitized fallback without touching history.
    int frame=firstFrame+frames;
    immediate->UpdateSubresource(output.Get(),0,nullptr,zeros.data(),outW*8,0);
    stages(frame);fe(frame,c);
    if(RiskOfRainUpscaling_UpscalerState(c)!=(hardware?2:-2) || !hdrOutput(peak) || Read(device,immediate,color.Get())!=scene)return 192;
    // No scene recorded for that frame: the previous output is kept.
    immediate->UpdateSubresource(output.Get(),0,nullptr,zeros.data(),outW*8,0);
    fe(frame+100,c);if(Read(device,immediate,output.Get())!=zeros)return 193;
    // Tail RCAS sharpens the display-sized post-processed output in place.
    std::vector<uint16_t> sdr(zeros.size());
    for(size_t i=0;i<sdr.size();++i)sdr[i]=XMConvertFloatToHalf(i%4==3?static_cast<float>((i/4)%5)/4:((i/4)%13)<6?.8f:.1f);
    immediate->UpdateSubresource(output.Get(),0,nullptr,sdr.data(),outW*8,0);
    if(RiskOfRainUpscaling_SetSharpness(0))return 194;
    se(frame,c);if(Read(device,immediate,output.Get())!=sdr)return 195;
    if(RiskOfRainUpscaling_SetSharpness(.7f))return 194;
    se(frame+1,c);auto sharpened=Read(device,immediate,output.Get());
    if(sharpened.size()!=sdr.size() || sharpened==sdr)return 196;
    for(size_t i=0;i<sharpened.size();++i){if(!std::isfinite(XMConvertHalfToFloat(sharpened[i])))return 197;if(i%4==3 && sharpened[i]!=sdr[i])return 198;}
    if(hardware){
        // Missing camera packet: latch Off, scene untouched, bilinear output.
        immediate->UpdateSubresource(output.Get(),0,nullptr,zeros.data(),outW*8,0);
        stages(frame+2);ce(frame+2,c);
        if(RiskOfRainUpscaling_UpscalerState(c)!=-2 || Read(device,immediate,color.Get())!=scene || !hdrOutput(peak))return 199;
        if(chain && FAILED(chain->Present(0,0)))return 188;
    }
    if(int failed=OutlineMaskFixture(device,immediate,c,inW,inH,outW,outH,frame+3))return failed;
    if(int failed=FullResolutionOutlineFixture(device,immediate,c,depth.Get(),inW,inH,outW,outH,frame+4))return failed;
    ce(-1,c);if(RiskOfRainUpscaling_UpscalerState(c)!=-1)return 200;RiskOfRainUpscaling_UpscalerDestroy(c);be(199,bc);RiskOfRainUpscaling_SceneInputsDestroy(bc);
    if(hardware)printf("PASS: %d DLSS Quality evaluations before post-processing %dx%d -> %dx%d (HDR peak %.2f), scene read-only, original alpha, temporal outline weight through DLSS alpha (border %.2f), bilinear fallback event, tail RCAS on the output and safe failure.\n",frames,inW,inH,outW,outH,evaluatedPeak,reconstructedBorder);
    else printf("PASS: before-post-processing upscaling without a supported runtime gives a sanitized bilinear HDR output (peak %.2f) and leaves the scene bit-exact; fallback/RCAS events validated.\n",evaluatedPeak);
    return 0;
}
int main(int argc,char** argv) {
    for(float invalid:{-0.1f,1.1f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()})
        if(RiskOfRainUpscaling_SetSharpness(invalid)!=-1)return 110;
    if(!FrameGenerationSpaceCheck())return 141;
    bool hardware=false,bridge=false,fg=false,visible=false,appWaitable=false,reflexLimit=false,fgLive=false;
    bool uiRecomposition=false,fsr=false;
    for(int i=1;i<argc;++i)uiRecomposition|=!std::strcmp(argv[i],"--ui-recomposition");
    for(int i=1;i<argc;++i)fsr|=!std::strcmp(argv[i],"--fsr");
    for(int i=1;i<argc;++i)fsrUpscaler|=!std::strcmp(argv[i],"--fsr-upscaler");
    if(fsrUpscaler && RiskOfRainUpscaling_SetUpscalerBackend(2))return 148;
    int requestedMultiplier=2,presentsAt32=0,maxSdkFrames=0;
    const int liveMultipliers[]{2,3,4,5,6,5,3,2};
    int blockPresents=0,blockMaxBatch=0,liveMaxMultiplier=2;
    for(int i=1;i<argc;++i){hardware|=!std::strcmp(argv[i],"--hardware");bridge|=!std::strcmp(argv[i],"--bridge");fg|=!std::strcmp(argv[i],"--fg");visible|=!std::strcmp(argv[i],"--visible");appWaitable|=!std::strcmp(argv[i],"--app-waitable");}
    for(int i=1;i<argc;++i)if(!std::strcmp(argv[i],"--fg-multiplier")){
        if(++i>=argc)return 123;requestedMultiplier=std::atoi(argv[i]);
        if(requestedMultiplier<2 || requestedMultiplier>6)return 123;
    }
    if(fg){hardware=bridge=true;width=1280;height=720;ConfigureFrameGeneration(true,requestedMultiplier,fsr?FrameGenerationBackend::FSR:FrameGenerationBackend::Auto);}
    for(int i=1;i<argc;++i)reflexLimit|=!std::strcmp(argv[i],"--reflex-limit");
    for(int i=1;i<argc;++i)fgLive|=!std::strcmp(argv[i],"--fg-live");
    if(fgLive && (!fg || reflexLimit || requestedMultiplier!=2))return 125;
    if(reflexLimit && !fg)return 90;
    if(bridge && !hardware)return 60;
    wchar_t current[32768]{};GetCurrentDirectoryW(32768,current);std::wstring root=current;
    RiskOfRainUpscaling_SetLoggingLevel(4); // Detailed fixture evidence.
    RiskOfRainUpscaling_Initialize((root+L"\\build\\test-results").c_str(),(root+L"\\build\\package\\BepInEx\\plugins\\RiskOfRainUpscaling\\runtimes").c_str(),L"pipeline-test");
    // --profile: cost profiler smoke run; build/test-results/profile.log.
    for(int i=1;i<argc;++i)if(!std::strcmp(argv[i],"--profile")){
        RiskOfRainUpscaling_ProfilerSetContext("pipeline fixture");RiskOfRainUpscaling_ProfilerSetEnabled(1);
        std::atexit([]{RiskOfRainUpscaling_ProfilerSetEnabled(0);});
    }
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> immediate;
    if(FAILED(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&immediate)))return 1;
    HWND window=nullptr;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGISwapChain1> chain;
    DXGI_SWAP_CHAIN_DESC1 chainDesc{};
    HANDLE applicationWait=nullptr;
    auto createChain=[&](){return bridge?CreateFrameBridge(factory.Get(),device.Get(),window,&chainDesc,nullptr,nullptr,&chain):factory->CreateSwapChainForHwnd(device.Get(),window,&chainDesc,nullptr,nullptr,&chain);};
    if(hardware) {
        window=CreateWindowExW(0,L"STATIC",L"RiskOfRainUpscaling DLAA presentation fixture",WS_OVERLAPPED,0,0,width,height,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!window || FAILED(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory))))return 31;
        chainDesc.Width=width;chainDesc.Height=height;chainDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        chainDesc.SampleDesc.Count=1;chainDesc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;chainDesc.BufferCount=2;chainDesc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        if(fg && appWaitable)chainDesc.Flags=DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH|DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING|DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        if(FAILED(createChain()))return 32;
        ObserveChain(chain.Get()); // Exactly the production observer/hook path.
        if(fg && visible){ShowWindow(window,SW_SHOW);UpdateWindow(window);}
        if(fg){
            ComPtr<IDXGISwapChain2> chain2;if(FAILED(chain.As(&chain2)))return 86;
            applicationWait=chain2->GetFrameLatencyWaitableObject();
            if(appWaitable){if(!applicationWait || FAILED(chain2->SetMaximumFrameLatency(2)))return 87;}
            else if(applicationWait || chain2->SetMaximumFrameLatency(2)!=DXGI_ERROR_INVALID_CALL)return 88;
        }
    }
    std::vector<uint16_t> pattern(width*height*4);
    using namespace DirectX::PackedVector;
    for(int y=0;y<height;++y)for(int x=0;x<width;++x){auto i=(y*width+x)*4;
        pattern[i]=XMConvertFloatToHalf((x%13)<6?.8f:.1f);pattern[i+1]=XMConvertFloatToHalf((y%11)<5?.6f:.15f);pattern[i+2]=XMConvertFloatToHalf(.3f);pattern[i+3]=XMConvertFloatToHalf(static_cast<float>((x+y)%7)/6);}
    auto color=Texture(device.Get(),DXGI_FORMAT_R16G16B16A16_TYPELESS,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET,pattern.data(),width*8);
    auto output=Texture(device.Get(),DXGI_FORMAT_R16G16B16A16_TYPELESS);
    // The game's raw inputs: Unity RFloat depth and RGHalf motion (the other fixtures keep legacy RGBA32F).
    auto depth=Texture(device.Get(),DXGI_FORMAT_R32_TYPELESS),motion=Texture(device.Get(),DXGI_FORMAT_R16G16_TYPELESS);
    if(!color||!output||!depth||!motion)return 2;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R16G16_FLOAT;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
    ComPtr<ID3D11ShaderResourceView> motionSRV;if(FAILED(device->CreateShaderResourceView(motion.Get(),&sd,&motionSRV)))return 3;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};rd.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;rd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> rtv;if(FAILED(device->CreateRenderTargetView(color.Get(),&rd,&rtv)))return 4;
    D3D11_RENDER_TARGET_VIEW_DESC md{};md.Format=DXGI_FORMAT_R16G16_FLOAT;md.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> motionRTV;if(FAILED(device->CreateRenderTargetView(motion.Get(),&md,&motionRTV)))return 5;
    float zero[4]{};immediate->ClearRenderTargetView(motionRTV.Get(),zero);
    std::vector<float> depths(width*height,.5f);
    auto sceneDepth=Texture(device.Get(),DXGI_FORMAT_R32_TYPELESS,D3D11_BIND_DEPTH_STENCIL,depths.data(),width*4);
    D3D11_DEPTH_STENCIL_VIEW_DESC ds{};ds.Format=DXGI_FORMAT_D32_FLOAT;ds.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11DepthStencilView> dsv;if(!sceneDepth||FAILED(device->CreateDepthStencilView(sceneDepth.Get(),&ds,&dsv)))return 6;
    SceneInputsDescription b{sizeof(b),4,width,height,depth.Get()};
    auto bc=RiskOfRainUpscaling_SceneInputsCreate(&b);if(!bc)return 7;
    UpscalerDescription d{sizeof(d),2,width,height,bc,color.Get(),depth.Get(),motion.Get(),output.Get(),width,height,0,0};
    auto invalid=d;invalid.output=invalid.color;if(RiskOfRainUpscaling_UpscalerCreate(&invalid))return 8;
    invalid=d;invalid.width=width/2;if(RiskOfRainUpscaling_UpscalerCreate(&invalid))return 9;
    invalid=d;invalid.quality=1;if(RiskOfRainUpscaling_UpscalerCreate(&invalid))return 9; // Upscaling modes need a smaller render size.
    invalid=d;invalid.version=1;if(RiskOfRainUpscaling_UpscalerCreate(&invalid))return 9;
    auto c=RiskOfRainUpscaling_UpscalerCreate(&d);if(!c)return 10;
    auto ce=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_UpscalerGetEvaluateEvent());
    auto be=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_SceneInputsGetEvent());
    auto bf=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_SceneInputsGetFrameEvent());
    auto f=Frame(1),bad=f;bad.jitterY=std::numeric_limits<float>::quiet_NaN();if(RiskOfRainUpscaling_UpscalerSubmitFrame(c,&bad)==0)return 11;
    if(RiskOfRainUpscaling_UpscalerSubmitFrame(c,&f)||RiskOfRainUpscaling_UpscalerSubmitFrame(c,&f)==0)return 12;
    auto target=rtv.Get();immediate->OMSetRenderTargets(1,&target,dsv.Get());
    D3D11_VIEWPORT viewport{3,4,5,6,0,1};immediate->RSSetViewports(1,&viewport);
    ce(-2,c);
    ComPtr<ID3D11RenderTargetView> restored;immediate->OMGetRenderTargets(1,&restored,nullptr);
    if(restored.Get()!=rtv.Get())return 13;
    if(!hardware) {
        if(RiskOfRainUpscaling_UpscalerState(c)!=-2)return 14;
        ce(1,c);if(Read(device.Get(),immediate.Get(),output.Get())!=pattern)return 15;
        printf("PASS: DLAA WARP rejects unsupported hardware, copies exact original fallback, preserves bindings, validates ABI/alias/frame packets.\n");
    } else {
        if(RiskOfRainUpscaling_UpscalerState(c)!=1)return 16;
        // DXGI vtables are shared. An unobserved overlay chain must keep its
        // normal native path even when it uses the same D3D11 device.
        HWND otherWindow=CreateWindowExW(0,L"STATIC",L"Unowned presentation fixture",WS_OVERLAPPED,0,0,width,height,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        ComPtr<IDXGISwapChain1> otherChain;
        if(!otherWindow || FAILED(factory->CreateSwapChainForHwnd(device.Get(),otherWindow,&chainDesc,nullptr,nullptr,&otherChain)))return 52;
        unsigned beforeUnowned=StreamlinePresentedFrames();
        if(FAILED(otherChain->Present(0,0)) || StreamlinePresentedFrames()!=beforeUnowned)return 53;
        auto otherRaw=otherChain.Detach();if(otherRaw->Release()!=0)return 54;DestroyWindow(otherWindow);
        // More than 60 consecutive evaluations would trigger the SDK's missing-
        // present warning if common bookkeeping is still absent.
        double pacedTimes[129]{};
        for(int i=1;i<=128;++i) {
            if(fgLive && (i-1)%16==0){
                requestedMultiplier=liveMultipliers[(i-1)/16];
                int state=RiskOfRainUpscaling_FrameGenerationState(),previous=RiskOfRainUpscaling_FrameGenerationMultiplier();
                if(RiskOfRainUpscaling_FrameGenerationSetMultiplier(requestedMultiplier)!=requestedMultiplier ||
                   RiskOfRainUpscaling_FrameGenerationSetMultiplier(0)!=-1 || RiskOfRainUpscaling_FrameGenerationSetMultiplier(7)!=-1 ||
                   RiskOfRainUpscaling_FrameGenerationState()!=state || RiskOfRainUpscaling_FrameGenerationMultiplier()!=previous || RiskOfRainUpscaling_GetPresentationState()!=2)return 126;
                blockPresents=RiskOfRainUpscaling_FrameGenerationPresentedFrames();blockMaxBatch=0;
            }
            // Exercise live Off -> strongest -> light changes on one DLSS context.
            if(i==32 && RiskOfRainUpscaling_SetSharpness(0.f))return 111;
            if(i==64 && RiskOfRainUpscaling_SetSharpness(1.f))return 112;
            if(i==96 && RiskOfRainUpscaling_SetSharpness(0.2f))return 113;
            if(applicationWait && WaitForSingleObject(applicationWait,5000)!=WAIT_OBJECT_0)return 89;
            if(fg && visible){
                MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
                HWND foreground=GetForegroundWindow();DWORD foregroundThread=GetWindowThreadProcessId(foreground,nullptr);
                BOOL attached=foregroundThread && foregroundThread!=GetCurrentThreadId()?AttachThreadInput(GetCurrentThreadId(),foregroundThread,TRUE):FALSE;
                SetForegroundWindow(window);SetFocus(window);
                if(attached)AttachThreadInput(GetCurrentThreadId(),foregroundThread,FALSE);
                if(i==1)printf("FG_FIXTURE_WINDOW ours=%p foreground=%p active=%p visible=%d\n",window,GetForegroundWindow(),GetActiveWindow(),IsWindowVisible(window));
            }
            if(reflexLimit){
                if(i==97)RiskOfRainUpscaling_FrameGenerationSetEnabled(0);
                // FSR's swap-chain pacing stays slower after a 30 FPS stretch in this
                // readback-bound fixture, so FSR measures the 60 cap before the 30 cap.
                int cap=fsr?(i<=32?0:i<=64?16667:33333):(i<=32?0:i<=64 || i>=97?33333:16667);
                if(RiskOfRainUpscaling_ReflexSetFrameLimit(cap)!=cap || RiskOfRainUpscaling_ReflexFrameLimit()!=cap)return 91;
                if(RiskOfRainUpscaling_ReflexSetFrameLimit(-1)!=-1 || RiskOfRainUpscaling_ReflexFrameLimit()!=cap)return 92;
                if(RiskOfRainUpscaling_ReflexSetFrameLimit(1000001)!=-1 || RiskOfRainUpscaling_ReflexFrameLimit()!=cap)return 98;
            }
            if(fg){RiskOfRainUpscaling_FrameGenerationMarker(0,i);RiskOfRainUpscaling_FrameGenerationMarker(1,i);RiskOfRainUpscaling_FrameGenerationMarker(2,i);}
            pacedTimes[i]=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            f=Frame(i);if(i>1 && RiskOfRainUpscaling_UpscalerSubmitFrame(c,&f))return 17;
            bf(i,bc);immediate->OMSetRenderTargets(1,&target,dsv.Get());be(90,bc);be(100,bc);
            auto srv=motionSRV.Get();immediate->PSSetShaderResources(0,1,&srv);be(102,bc);
            ce(i,c);if(RiskOfRainUpscaling_UpscalerState(c)!=2 || RiskOfRainUpscaling_UpscalerKind(c)!=(fsrUpscaler?2:1))return 18;
            restored.Reset();immediate->OMGetRenderTargets(1,&restored,nullptr);
            D3D11_VIEWPORT vp{};UINT count=1;immediate->RSGetViewports(&count,&vp);
            if(restored.Get()!=rtv.Get() || count!=1 || vp.TopLeftX!=3 || vp.Width!=5)return 19;
            unsigned before=StreamlinePresentedFrames();
            std::vector<unsigned char> bridgeImage;
            if(bridge && (fg || i==1 || i==64)){
                // Synthetic fixture conversion; Unity uses its normal final blit.
                auto evaluated=Read(device.Get(),immediate.Get(),output.Get());
                if(evaluated.size()!=pattern.size())return 63;
                bridgeImage.resize(evaluated.size());
                for(size_t p=0;p<evaluated.size();++p)bridgeImage[p]=static_cast<unsigned char>(std::clamp(XMConvertHalfToFloat(evaluated[p]),0.f,1.f)*255.f+.5f);
                ComPtr<ID3D11Texture2D> producer;if(FAILED(chain->GetBuffer(0,IID_PPV_ARGS(&producer))))return 64;
                immediate->UpdateSubresource(producer.Get(),0,nullptr,bridgeImage.data(),width*4,0);
            }
            if(fg && uiRecomposition){
                if(!DrawUiFixture(device.Get(),immediate.Get(),chain.Get(),i))return 142;
            }else if(fg){
                auto event=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_FrameGenerationGetBeforeHudEvent());event(-i,nullptr);event(i,nullptr);
                // Draw a synthetic HUD after the snapshot. It must not leak into the tagged color.
                ComPtr<ID3D11Texture2D> producer;if(FAILED(chain->GetBuffer(0,IID_PPV_ARGS(&producer))))return 80;
                std::vector<unsigned char> hud(16*12*4,255);D3D11_BOX box{0,0,0,16,12,1};
                immediate->UpdateSubresource(producer.Get(),0,&box,hud.data(),16*4,0);
            }
            if(i==1) {
                if(FAILED(chain->Present(0,DXGI_PRESENT_TEST)) || StreamlinePresentedFrames()!=before)return 33;
            }
            DXGI_PRESENT_PARAMETERS params{};
            int sdkBefore=fg?RiskOfRainUpscaling_FrameGenerationPresentedFrames():0;
            HRESULT presented=(i%2)?chain->Present(0,0):chain->Present1(0,0,&params);
            if(FAILED(presented) || StreamlinePresentedFrames()!=before+PresentStep())return 34;
            if(fg && !reflexLimit){
                maxSdkFrames=std::max(maxSdkFrames,RiskOfRainUpscaling_FrameGenerationPresentedFrames()-sdkBefore);
                if(fgLive){
                    blockMaxBatch=std::max(blockMaxBatch,RiskOfRainUpscaling_FrameGenerationPresentedFrames()-sdkBefore);
                    if(i%16==0){
                        int effective=RiskOfRainUpscaling_FrameGenerationMultiplier(),actual=RiskOfRainUpscaling_FrameGenerationPresentedFrames()-blockPresents;
                        if(requestedMultiplier==6)liveMaxMultiplier=effective;
                        printf("FG_LIVE_MULTIPLIER requested=%dx effective=%dx baseFrames=16 SDK presents=%d maxBatch=%d\n",requestedMultiplier,effective,actual,blockMaxBatch);
                        if(effective<2 || effective>requestedMultiplier || blockMaxBatch<effective || actual<16+effective-1 || RiskOfRainUpscaling_FrameGenerationState()!=3)return 127;
                    }
                }
                if(i==32)presentsAt32=RiskOfRainUpscaling_FrameGenerationPresentedFrames();
                if(i==64){
                    int multiplier=RiskOfRainUpscaling_FrameGenerationMultiplier(),actual=RiskOfRainUpscaling_FrameGenerationPresentedFrames()-presentsAt32;
                    printf("FG_MULTIPLIER_MEASURED requested=%dx effective=%dx baseFrames=32 SDK presents=%d\n",requestedMultiplier,multiplier,actual);
                }
            }
            if(fg && (i==4 || i==64)){
                std::vector<unsigned char> readback(width*height*4);
                if(FAILED(FrameBridgeInputReadback(chain.Get(),3,readback.data(),static_cast<unsigned>(readback.size()))) || readback!=bridgeImage)return 81;
                std::vector<float> readDepth(width*height);
                if(FAILED(FrameBridgeInputReadback(chain.Get(),1,readDepth.data(),static_cast<unsigned>(readback.size()))))return 82;
                for(auto sample:readDepth)if(sample!=.5f)return 83;
                std::vector<uint16_t> readMotion(width*height*2);
                if(FAILED(FrameBridgeInputReadback(chain.Get(),2,readMotion.data(),static_cast<unsigned>(readback.size()))))return 84;
                for(auto m:readMotion)if(XMConvertHalfToFloat(m)!=0.f)return 85;
                if(uiRecomposition && !fsr){
                    std::vector<unsigned char> alpha(width*height);
                    if(FAILED(FrameBridgeInputReadback(chain.Get(),4,alpha.data(),static_cast<unsigned>(alpha.size()))))return 143;
                    for(int y=0;y<height;++y)for(int x=0;x<width;++x)if(alpha[y*width+x]!=(x<16 && y<12?255:0))return 144;
                    printf("FG_UI_ALPHA_VERIFIED unityFrame=%d display-size R8; HUD rectangle opaque; scene opacity excluded\n",i);
                }
                if(fsr){
                    // FSR presents the HUD-less image and composites this premultiplied layer.
                    std::vector<unsigned char> layer(width*height*4);
                    if(FAILED(FrameBridgeInputReadback(chain.Get(),5,layer.data(),static_cast<unsigned>(layer.size()))))return 145;
                    for(int y=0;y<height;++y)for(int x=0;x<width;++x)for(int k=0;k<4;++k)if(layer[(y*width+x)*4+k]!=(x<16 && y<12?255:0))return 149;
                    printf("FG_UI_LAYER_VERIFIED unityFrame=%d %s; HUD rectangle opaque white; scene pixels empty\n",i,uiRecomposition?"measured opacity":"difference mask");
                }
            }
            if(!fg && !bridgeImage.empty()){
                std::vector<unsigned char> displayed(bridgeImage.size());
                if(FAILED(FrameBridgeReadback(chain.Get(),displayed.data(),static_cast<unsigned>(displayed.size()))) || displayed!=bridgeImage)return 65;
            }
            restored.Reset();immediate->OMGetRenderTargets(1,&restored,nullptr);
            if(restored.Get()!=rtv.Get())return 35;
            if(i==64 && !fg) {
                ComPtr<ID3D11Texture2D> buffer;
                if(FAILED(chain->GetBuffer(0,IID_PPV_ARGS(&buffer))))return 36;
                buffer.Reset();
                if(FAILED(chain->ResizeBuffers(2,width+16,height+16,DXGI_FORMAT_UNKNOWN,0)))return 37;
                if(bridge){ComPtr<IDXGISwapChain2> chain2;if(FAILED(chain.As(&chain2)) || RiskOfRainUpscaling_GetPresentationState()!=2)return 61;}
                else{ComPtr<IDXGISwapChain3> chain3;if(FAILED(chain.As(&chain3)) || chain3->GetCurrentBackBufferIndex()>1)return 38;}
                if(FAILED(chain->SetFullscreenState(FALSE,nullptr)))return 38;
            }
        }
        if(fg){
            if(reflexLimit){
                const double early=24/(pacedTimes[62]-pacedTimes[38]),late=24/(pacedTimes[94]-pacedTimes[70]);
                double fps30=fsr?late:early,fps60=fsr?early:late;
                double fpsOff30=24/(pacedTimes[126]-pacedTimes[102]);
                if(RiskOfRainUpscaling_ReflexSetFrameLimit(0)!=0)return 99;
                printf("REFLEX_LIMITER_MEASURED baseFPS cap30=%.2f cap60=%.2f FGoffCap30=%.2f clearedUs=%d guardWaits=%d appWaitable=%d\n",fps30,fps60,fpsOff30,RiskOfRainUpscaling_ReflexFrameLimit(),RiskOfRainUpscaling_ReflexGuardWaits(),appWaitable);
                if(fps30<24 || fps30>36 || fps60<48 || fps60>72 || fpsOff30<24 || fpsOff30>36 || RiskOfRainUpscaling_ReflexFrameLimit()!=0)return 93;
            }
            if(RiskOfRainUpscaling_FrameGenerationKind()!=(fsr?2:1)){printf("FG backend=%d expected=%d\n",RiskOfRainUpscaling_FrameGenerationKind(),fsr?2:1);return 147;}
            if(RiskOfRainUpscaling_FrameGenerationState()!=(reflexLimit?0:3) || RiskOfRainUpscaling_FrameGenerationPresentedFrames()<180){printf("FG verification failed state=%d SDK presents=%d\n",RiskOfRainUpscaling_FrameGenerationState(),RiskOfRainUpscaling_FrameGenerationPresentedFrames());return 66;}
            if(!reflexLimit){
                int multiplier=RiskOfRainUpscaling_FrameGenerationMultiplier();
                // State counts asynchronous presentations since the last query.
                // Allow transient focus/warmup skips, but require sustained extra
                // presentations and a batch reaching the selected multiplier.
                printf("FG_MULTIPLIER_VERIFIED requested=%dx effective=%dx baseFrames=128 SDK presents=%d maxBatch=%d\n",requestedMultiplier,multiplier,RiskOfRainUpscaling_FrameGenerationPresentedFrames(),maxSdkFrames);
                if(multiplier<2 || multiplier>requestedMultiplier || maxSdkFrames<multiplier || RiskOfRainUpscaling_FrameGenerationPresentedFrames()<128*multiplier*7/10)return 124;
            }
            auto fe=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_FrameGenerationGetBeforeHudEvent());
            auto timing=[&](int frame){RiskOfRainUpscaling_FrameGenerationMarker(0,frame);RiskOfRainUpscaling_FrameGenerationMarker(1,frame);RiskOfRainUpscaling_FrameGenerationMarker(2,frame);fe(-frame,nullptr);};
            RiskOfRainUpscaling_FrameGenerationSetEnabled(0);
            if(reflexLimit && RiskOfRainUpscaling_ReflexSetFrameLimit(33333)!=33333)return 94;
            timing(129);
            if(FAILED(chain->Present(0,0)) || RiskOfRainUpscaling_FrameGenerationState()!=0 || RiskOfRainUpscaling_GetPresentationState()!=2)return 67;
            if(fgLive){requestedMultiplier=4;if(RiskOfRainUpscaling_FrameGenerationSetMultiplier(4)!=4 || RiskOfRainUpscaling_FrameGenerationState()!=0)return 128;}
            if(reflexLimit && (RiskOfRainUpscaling_ReflexFrameLimit()!=33333 || RiskOfRainUpscaling_ReflexSetFrameLimit(0)!=0))return 95;
            RiskOfRainUpscaling_FrameGenerationSetEnabled(1);
            timing(130);
            if(FAILED(chain->Present(0,0)) || RiskOfRainUpscaling_FrameGenerationState()!=2)return 68; // Missing inputs cannot reuse the last gameplay packet.
            auto evaluate=[&](int frame,int hudFrame){
                timing(frame);f=Frame(frame);if(RiskOfRainUpscaling_UpscalerSubmitFrame(c,&f))return false;
                bf(frame,bc);immediate->OMSetRenderTargets(1,&target,dsv.Get());be(90,bc);be(100,bc);
                auto srv=motionSRV.Get();immediate->PSSetShaderResources(0,1,&srv);be(102,bc);ce(frame,c);
                if(RiskOfRainUpscaling_UpscalerState(c)!=2)return false;
                ComPtr<ID3D11Texture2D> producer;if(FAILED(chain->GetBuffer(0,IID_PPV_ARGS(&producer))))return false;
                auto out=Read(device.Get(),immediate.Get(),output.Get());std::vector<unsigned char> converted(out.size());
                for(size_t p=0;p<out.size();++p)converted[p]=static_cast<unsigned char>(std::clamp(XMConvertHalfToFloat(out[p]),0.f,1.f)*255.f+.5f);
                immediate->UpdateSubresource(producer.Get(),0,nullptr,converted.data(),width*4,0);producer.Reset();
                fe(hudFrame,nullptr);return SUCCEEDED(chain->Present(0,0));
            };
            if(!evaluate(131,131) || RiskOfRainUpscaling_FrameGenerationState()!=3)return 69;
            if(RiskOfRainUpscaling_GetPresentationState()!=2)return 132;
            if(fgLive && RiskOfRainUpscaling_FrameGenerationMultiplier()!=std::min(4,liveMaxMultiplier))return 130;
            if(!evaluate(132,133) || RiskOfRainUpscaling_FrameGenerationState()!=2)return 70; // Same-frame scene/HUD check must reject this pair.
            if(fgLive){requestedMultiplier=6;if(RiskOfRainUpscaling_FrameGenerationSetMultiplier(6)!=6 || RiskOfRainUpscaling_FrameGenerationState()!=2)return 129;}
            if(!evaluate(133,133) || RiskOfRainUpscaling_FrameGenerationState()!=3)return 71;
            if(fgLive && RiskOfRainUpscaling_FrameGenerationMultiplier()!=liveMaxMultiplier)return 131;
            immediate->ClearState();immediate->Flush();restored.Reset();
            if(FAILED(chain->ResizeBuffers(2,width+16,height+16,DXGI_FORMAT_UNKNOWN,0)) || RiskOfRainUpscaling_FrameGenerationState()!=2)return 72;
            timing(134);if(FAILED(chain->Present(0,0)) || RiskOfRainUpscaling_FrameGenerationState()!=2)return 73;
            if(FAILED(chain->ResizeBuffers(2,width,height,DXGI_FORMAT_UNKNOWN,0)))return 74;
            if(!evaluate(135,135) || RiskOfRainUpscaling_FrameGenerationState()!=3)return 75;
            ce(-1,c);RiskOfRainUpscaling_UpscalerDestroy(c);be(199,bc);RiskOfRainUpscaling_SceneInputsDestroy(bc);
            if(int failed=UpscaleFixture(device.Get(),immediate.Get(),true,chain.Get(),true,140))return failed;
            auto raw=chain.Detach();if(raw->Release()!=0)return 76;
            immediate->ClearState();immediate->Flush();
            if(FAILED(createChain()))return 77;ObserveChain(chain.Get());timing(136);
            if(FAILED(chain->Present(0,0)) || RiskOfRainUpscaling_FrameGenerationState()<0)return 78;
            if(reflexLimit){
                if(RiskOfRainUpscaling_ReflexSetFrameLimit(33333)!=33333)return 96;
                FrameGenerationFail("Intentional fixture failure to test Reflex cap release");
                if(RiskOfRainUpscaling_ReflexFrameLimit()!=-1 || RiskOfRainUpscaling_ReflexSetFrameLimit(0)!=0 || RiskOfRainUpscaling_ReflexSetFrameLimit(33333)!=-1)return 97;
                printf("PASS: Reflex plus native interval guard paces 30/60 rendered FPS, unlimited clears, FG Off keeps pacing, invalid cap rejected, failed runtime releases cap.\n");
            }
            raw=chain.Detach();if(raw->Release()!=0)return 79;DestroyWindow(window);
            printf("PASS: simultaneous D3D11 DLAA / D3D12 DLSSG requested=%dx effective=%dx, actual SDK presents=%d, six exact depth/motion/pre-UI GPU readbacks with synthetic HUD exclusion, input-completion fences, Off/On, missing/mismatched input suspension, active resize and resident-SDK chain recreation. Unity timing/quality remain game tests.\n",requestedMultiplier,RiskOfRainUpscaling_FrameGenerationMultiplier(),RiskOfRainUpscaling_FrameGenerationPresentedFrames());
            return 0;
        }
        auto pixels=Read(device.Get(),immediate.Get(),output.Get());if(pixels.size()!=pattern.size())return 20;
        double sum=0;for(size_t i=0;i<pixels.size();++i)if(i%4!=3){float v=XMConvertHalfToFloat(pixels[i]);if(!std::isfinite(v))return 21;sum+=v;}
        for(size_t i=3;i<pixels.size();i+=4)if(pixels[i]!=pattern[i])return 30;
        if(sum<100)return 22;
        // Streamline's common after-hook returns S_OK; Unity must still see the
        // original DXGI error, rather than silently accepting a failed Present.
        unsigned beforeInvalid=StreamlinePresentedFrames();
        if(chain->Present(5,0)!=DXGI_ERROR_INVALID_CALL || StreamlinePresentedFrames()!=beforeInvalid)return 45;
        // Missing same-frame camera packet must fail safely even after real evaluation.
        ce(129,c);if(RiskOfRainUpscaling_UpscalerState(c)!=-2||Read(device.Get(),immediate.Get(),output.Get())!=pattern)return 23;
        printf("PASS: 128 hardware DLAA evaluations through both Present APIs, TEST filtering, resize, native graphics-state restoration, original-alpha preservation and failure fallback. Hidden synthetic window; game quality unproven.\n");
    }
    ce(-1,c);if(RiskOfRainUpscaling_UpscalerState(c)!=-1)return 24;RiskOfRainUpscaling_UpscalerDestroy(c);
    if(!hardware) if(int failed=UpscaleFixture(device.Get(),immediate.Get(),false,nullptr))return failed;
    if(!hardware) if(int failed=PrePostFixture(device.Get(),immediate.Get(),false,nullptr,1))return failed;
    if(!hardware) if(int failed=PrePostUpscaleFixture(device.Get(),immediate.Get(),false,nullptr,1))return failed;
    if(hardware) {
        auto before=StreamlinePresentedFrames();
        if(FAILED(chain->Present(0,0)) || StreamlinePresentedFrames()!=before+PresentStep())return 39;
        // Recreate while SDK coverage is healthy, then require the new chain
        // to bind and service the same resident SDK before re-enabling DLAA.
        auto replaced=chain.Detach();if(replaced->Release()!=0)return 55;
        if(FAILED(createChain()))return 56;
        ObserveChain(chain.Get());before=StreamlinePresentedFrames();
        if(FAILED(chain->Present(0,0)) || StreamlinePresentedFrames()!=before+PresentStep())return 57;
        // No feature context is active here, and presentation coverage is healthy.
        if(int failed=UpscaleFixture(device.Get(),immediate.Get(),true,chain.Get()))return failed;
        if(int failed=PrePostFixture(device.Get(),immediate.Get(),true,chain.Get(),200))return failed;
        if(int failed=PrePostUpscaleFixture(device.Get(),immediate.Get(),true,chain.Get(),300))return failed;
        // Re-enable with the same owned SDK after retiring the old feature context.
        c=RiskOfRainUpscaling_UpscalerCreate(&d);if(!c)return 25;ce(-2,c);if(RiskOfRainUpscaling_UpscalerState(c)!=1)return 26;
        f=Frame(130);f.reset=1;if(RiskOfRainUpscaling_UpscalerSubmitFrame(c,&f))return 27;
        bf(130,bc);immediate->OMSetRenderTargets(1,&target,dsv.Get());be(90,bc);be(100,bc);
        auto srv=motionSRV.Get();immediate->PSSetShaderResources(0,1,&srv);be(102,bc);
        ce(130,c);if(RiskOfRainUpscaling_UpscalerState(c)!=2)return 28;
        if(FAILED(chain->Present(0,0)))return 40;
        if(fsrUpscaler){
            // FSR runs on its own D3D12 queue and has no presentation hooks to lose.
            ce(-1,c);if(RiskOfRainUpscaling_UpscalerState(c)!=-1)return 29;RiskOfRainUpscaling_UpscalerDestroy(c);
            printf("PASS: FSR re-enables and evaluates after resource retirement; D3D11 to D3D12 interop on a private queue.\n");
        }else{
        // Lose presentation coverage deliberately. Stop before the SDK would
        // emit its warning, and restore the exact original image.
        for(int i=131;i<=191;++i) {
            f=Frame(i);if(RiskOfRainUpscaling_UpscalerSubmitFrame(c,&f))return 46;
            bf(i,bc);immediate->OMSetRenderTargets(1,&target,dsv.Get());be(90,bc);be(100,bc);
            auto motionSource=motionSRV.Get();immediate->PSSetShaderResources(0,1,&motionSource);be(102,bc);
            ce(i,c);
            if(RiskOfRainUpscaling_UpscalerState(c)!=(i==191?-2:2))return 47;
        }
        if(Read(device.Get(),immediate.Get(),output.Get())!=pattern)return 48;
        ce(-1,c);if(RiskOfRainUpscaling_UpscalerState(c)!=-1)return 29;RiskOfRainUpscaling_UpscalerDestroy(c);
        printf("PASS: DLAA re-enables and evaluates after resource retirement without reinitializing the SDK.\n");
        printf("PASS: Losing presentation coverage stops DLAA before the SDK's 61-evaluation warning and copies exact original fallback.\n");
        c=RiskOfRainUpscaling_UpscalerCreate(&d);if(!c)return 49;ce(-2,c);
        if(RiskOfRainUpscaling_UpscalerState(c)!=-2)return 50;
        ce(-1,c);if(RiskOfRainUpscaling_UpscalerState(c)!=-1)return 51;RiskOfRainUpscaling_UpscalerDestroy(c);
        }
    }
    be(199,bc);RiskOfRainUpscaling_SceneInputsDestroy(bc);
    if(hardware) {
        auto raw=chain.Detach();if(raw->Release()!=0)return 41;
        // Reusing the same HWND must not find a retained old flip-model chain.
        if(FAILED(createChain()))return 42;
        ObserveChain(chain.Get());
        if(FAILED(chain->Present(0,0)))return 43;
        raw=chain.Detach();if(raw->Release()!=0)return 44;
        DestroyWindow(window);
        printf("PASS: SDK presentation cleanup continues while DLAA is Off; native/proxy references retire and the HWND accepts a replacement chain.\n");
        if(bridge && RiskOfRainUpscaling_GetPresentationState()==2)return 62;
        if(bridge)printf("PASS: Real D3D11 DLAA and SDK bookkeeping coexist with the native D3D12 frame bridge. FG OFF.\n");
    }
    return 0;
}
