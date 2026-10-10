#include "FrameGeneration/HudOpacity.h"
#include "shaders/Fullscreen.h"
#include "shaders/PreserveAlpha.h"
#include <vector>
#include <cstdio>
#include <stdexcept>
#include <cmath>

static void Require(HRESULT hr){if(FAILED(hr))throw std::runtime_error("HUD GPU fixture failed");}
static void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static std::vector<unsigned char> Read(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* texture,UINT bytes){
    D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;Require(device->CreateTexture2D(&d,nullptr,&staging));context->CopyResource(staging.Get(),texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};Require(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
    std::vector<unsigned char> result(d.Width*d.Height*bytes);
    for(UINT y=0;y<d.Height;++y)memcpy(result.data()+y*d.Width*bytes,static_cast<unsigned char*>(mapped.pData)+y*mapped.RowPitch,d.Width*bytes);
    context->Unmap(staging.Get(),0);return result;
}
int main(int argc,char** argv){
    try {
        const bool hardware=argc>1 && std::string(argv[1])=="--hardware";
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        Require(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
        const UINT w=31,h=19;
        D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.ArraySize=d.MipLevels=1;d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> target;Require(device->CreateTexture2D(&d,nullptr,&target));
        ComPtr<ID3D11RenderTargetView> rtv;Require(device->CreateRenderTargetView(target.Get(),nullptr,&rtv));
        std::vector<unsigned char> image(w*h*4);
        for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x){auto p=(y*w+x)*4;image[p]=128;image[p+1]=76;image[p+2]=204;image[p+3]=(x>4 && x<25 && y>2 && y<16)?128:0;}
        D3D11_SUBRESOURCE_DATA initial{image.data(),w*4,0};ComPtr<ID3D11Texture2D> input;Require(device->CreateTexture2D(&d,&initial,&input));
        ComPtr<ID3D11ShaderResourceView> srv;Require(device->CreateShaderResourceView(input.Get(),nullptr,&srv));
        ComPtr<ID3D11VertexShader> vertex;ComPtr<ID3D11PixelShader> pixel;
        Require(device->CreateVertexShader(FullscreenShader,sizeof(FullscreenShader),nullptr,&vertex));
        Require(device->CreatePixelShader(PreserveAlphaShader,sizeof(PreserveAlphaShader),nullptr,&pixel));
        D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.ScissorEnable=TRUE;
        ComPtr<ID3D11RasterizerState> raster;Require(device->CreateRasterizerState(&rd,&raster));
        d.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;d.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> depth;ComPtr<ID3D11DepthStencilView> dsv;Require(device->CreateTexture2D(&d,nullptr,&depth));Require(device->CreateDepthStencilView(depth.Get(),nullptr,&dsv));
        D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthFunc=D3D11_COMPARISON_ALWAYS;ds.StencilEnable=TRUE;ds.StencilReadMask=ds.StencilWriteMask=255;
        ds.FrontFace.StencilFunc=ds.BackFace.StencilFunc=D3D11_COMPARISON_EQUAL;
        ds.FrontFace.StencilFailOp=ds.BackFace.StencilFailOp=ds.FrontFace.StencilDepthFailOp=ds.BackFace.StencilDepthFailOp=D3D11_STENCIL_OP_KEEP;
        ds.FrontFace.StencilPassOp=ds.BackFace.StencilPassOp=D3D11_STENCIL_OP_INCR_SAT;
        ComPtr<ID3D11DepthStencilState> stencil;Require(device->CreateDepthStencilState(&ds,&stencil));
        D3D11_BLEND_DESC bd{};auto& b=bd.RenderTarget[0];b.BlendEnable=TRUE;b.SrcBlend=D3D11_BLEND_SRC_ALPHA;b.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;
        b.SrcBlendAlpha=D3D11_BLEND_SRC_ALPHA;b.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOp=b.BlendOpAlpha=D3D11_BLEND_OP_ADD;b.RenderTargetWriteMask=15;
        ComPtr<ID3D11BlendState> blend;Require(device->CreateBlendState(&bd,&blend));
        FLOAT factor[4]{.1f,.2f,.3f,.4f};FLOAT background[4]{.2f,.3f,.4f,.7f};D3D11_RECT rect{0,0,static_cast<LONG>(w),static_cast<LONG>(h)};
        D3D11_VIEWPORT vp{0,0,static_cast<float>(w),static_cast<float>(h),0,1};
        auto bind=[&]{auto r=rtv.Get();context->OMSetRenderTargets(1,&r,dsv.Get());context->OMSetBlendState(blend.Get(),factor,0xffffffff);context->OMSetDepthStencilState(stencil.Get(),1);
            context->RSSetViewports(1,&vp);context->RSSetState(raster.Get());context->RSSetScissorRects(1,&rect);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(vertex.Get(),nullptr,0);context->PSSetShader(pixel.Get(),nullptr,0);ID3D11ShaderResourceView* views[]{srv.Get(),srv.Get()};context->PSSetShaderResources(0,2,views);};
        HudOpacity capture(device.Get(),context.Get(),w,h);
        // Replay must not change the game's RGBA output or increment stencil:
        // the second draw fails the stencil test after the first original draw.
        bind();context->ClearRenderTargetView(rtv.Get(),background);context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,1);
        context->Draw(3,0);context->Draw(3,0);auto ordinary=Read(device.Get(),context.Get(),target.Get(),4);
        bind();context->ClearRenderTargetView(rtv.Get(),background);context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,1);
        capture.Begin(target.Get(),1);context->Draw(3,0);context->Draw(3,0);Check(capture.End(1),"standard HUD capture rejected");
        Check(Read(device.Get(),context.Get(),target.Get(),4)==ordinary,"capture changed original color/alpha or stencil");
        auto alpha=Read(device.Get(),context.Get(),capture.Texture(),1);
        for(UINT p=0;p<w*h;++p)if(std::abs(int(alpha[p])-int(image[p*4+3]))>1){printf("coverage pixel=%u alpha=%u expected=%u\n",p,alpha[p],image[p*4+3]);Check(false,"HUD opacity/stencil differs from original coverage");}
        ComPtr<ID3D11BlendState> restored;FLOAT restoredFactor[4]{};UINT restoredMask{};context->OMGetBlendState(&restored,restoredFactor,&restoredMask);
        Check(restored==blend && restoredMask==0xffffffff && memcmp(restoredFactor,factor,sizeof(factor))==0,"game blend state not restored");
        // Two overlapping translucent layers and a real scissor clip.
        D3D11_DEPTH_STENCIL_DESC noDepthDesc{};noDepthDesc.DepthFunc=D3D11_COMPARISON_ALWAYS;ComPtr<ID3D11DepthStencilState> noDepth;Require(device->CreateDepthStencilState(&noDepthDesc,&noDepth));
        rect.right=17;bind();context->OMSetDepthStencilState(noDepth.Get(),0);capture.Begin(target.Get(),2);
        context->Draw(3,0);context->Draw(3,0);Check(capture.End(2),"overlapping capture rejected");alpha=Read(device.Get(),context.Get(),capture.Texture(),1);
        for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x){auto p=y*w+x;float a=image[p*4+3]/255.f;int expected=x<17?int((1-(1-a)*(1-a))*255+.5f):0;if(std::abs(int(alpha[p])-expected)>1){printf("overlap pixel=%u alpha=%u expected=%d\n",p,alpha[p],expected);Check(false,"overlap/scissor opacity incorrect");}}
        // Premultiplied UI is accepted too. Additive/opaque custom blends fall back.
        b.SrcBlend=D3D11_BLEND_ONE;ComPtr<ID3D11BlendState> premult;Require(device->CreateBlendState(&bd,&premult));context->OMSetBlendState(premult.Get(),factor,0xffffffff);
        capture.Begin(target.Get(),3);context->Draw(3,0);Check(capture.End(3),"premultiplied UI rejected");
        b.DestBlend=D3D11_BLEND_ONE;ComPtr<ID3D11BlendState> additive;Require(device->CreateBlendState(&bd,&additive));context->OMSetBlendState(additive.Get(),factor,0xffffffff);
        capture.Begin(target.Get(),4);context->Draw(3,0);Check(!capture.End(4) && !capture.Texture(),"additive HUD did not fall back");
        bind();capture.Begin(target.Get(),5);context->Draw(3,0);Check(!capture.End(6) && !capture.Texture(),"mismatched frame published alpha");
        capture.Begin(target.Get(),7);Check(capture.End(7),"empty HUD rejected");alpha=Read(device.Get(),context.Get(),capture.Texture(),1);
        for(auto a:alpha)Check(a==0,"empty HUD included scene opacity");
        capture.Begin(target.Get(),8);capture.Cancel();Check(!capture.Texture(),"cancel published stale alpha");
        capture.Begin(target.Get(),9);context->ClearRenderTargetView(rtv.Get(),background);Check(!capture.End(9) && !capture.Texture(),"HUD clear published incomplete alpha");
        capture.Begin(target.Get(),10);context->UpdateSubresource(target.Get(),0,nullptr,image.data(),w*4,0);Check(!capture.End(10),"HUD direct update did not fall back");
        bind();context->OMSetDepthStencilState(noDepth.Get(),0);capture.Begin(target.Get(),11);context->DrawInstanced(3,2,0,0);Check(capture.End(11),"instanced HUD rejected");
        alpha=Read(device.Get(),context.Get(),capture.Texture(),1);
        for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x){auto p=y*w+x;float a=image[p*4+3]/255.f;int expected=x<17?int((1-(1-a)*(1-a))*255+.5f):0;Check(std::abs(int(alpha[p])-expected)<=1,"instanced HUD overlap incorrect");}
        printf("PASS: %s HUD opacity: real shader alpha, stencil/scissor clipping, overlapping transparency, premultiplied UI, unchanged game RGBA/state, additive fallback, empty HUD and frame/cancel guards.\n",hardware?"hardware":"WARP");return 0;
    }catch(const std::exception& e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
