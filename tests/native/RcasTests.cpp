#include "Core/Native.h"
#include "Upscaling/RcasSettings.h"
#include "shaders/Fullscreen.h"
#include "shaders/Rcas.h"
#include <array>
#include <vector>
#include <cstdio>
#include <stdexcept>
#include <cstring>

using Pixel=std::array<float,4>;
static void Require(HRESULT hr) {if(FAILED(hr))throw std::runtime_error("D3D11 RCAS fixture failure");}
// Match AMD's medium-precision reciprocal, rather than exact division. It is
// used by the unmodified filter for its noise estimate and final normalization.
static float MediumReciprocal(float value) {
    unsigned bits{};std::memcpy(&bits,&value,sizeof(bits));bits=0x7ef19fff-bits;
    float estimate{};std::memcpy(&estimate,&bits,sizeof(estimate));return estimate*(-estimate*value+2);
}
static Pixel Reference(const std::vector<Pixel>& image,int x,int y,int width,int height,float strength) {
    auto linear=image[y*width+x];
    auto sample=[&](int sx,int sy) {
        Pixel p=image[std::clamp(sy,0,height-1)*width+std::clamp(sx,0,width-1)];
        for(int k=0;k<3;++k)p[k]=std::sqrt(std::clamp(p[k],0.f,1.f));return p;
    };
    auto b=sample(x,y-1),d=sample(x-1,y),e=sample(x,y),f=sample(x+1,y),h=sample(x,y+1);
    if(strength==0 || (b==e && d==e && f==e && h==e))return linear;
    float lobe=-.1875f;
    for(int k=0;k<3;++k) {
        float lo=std::min({b[k],d[k],f[k],h[k]}),hi=std::max({b[k],d[k],f[k],h[k]});
        float hitMin=hi==0?0:std::min(lo,e[k])/(4*hi);
        float hitMax=lo==1?0:(1-std::max(hi,e[k]))/(4*lo-4);
        lobe=std::max(lobe,std::min(0.f,std::max(-hitMin,hitMax)));
    }
    auto luma=[](Pixel p){return p[2]*.5f+p[0]*.5f+p[1];};
    float bl=luma(b),dl=luma(d),el=luma(e),fl=luma(f),hl=luma(h);
    float range=std::max({bl,dl,el,fl,hl})-std::min({bl,dl,el,fl,hl});
    float nz=1-.5f*std::clamp(std::abs((bl+dl+fl+hl)*.25f-el)*MediumReciprocal(range),0.f,1.f);
    lobe*=strength*nz;
    Pixel out=linear;
    for(int k=0;k<3;++k) {
        float encoded=std::clamp((lobe*(b[k]+d[k]+f[k]+h[k])+e[k])*MediumReciprocal(4*lobe+1),0.f,1.f);
        out[k]=linear[k]+encoded*encoded-std::clamp(linear[k],0.f,1.f);
    }
    return out;
}
int main() {
    try {
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> immediate;
        Require(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&immediate));
        ComPtr<ID3D11VertexShader> vertex;ComPtr<ID3D11PixelShader> shader;
        Require(device->CreateVertexShader(FullscreenShader,sizeof(FullscreenShader),nullptr,&vertex));
        Require(device->CreatePixelShader(RcasShader,sizeof(RcasShader),nullptr,&shader));
        D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;
        ComPtr<ID3D11RasterizerState> rasterizer;Require(device->CreateRasterizerState(&raster,&rasterizer));
        for(auto size:std::initializer_list<std::pair<int,int>>{{17,11},{1,1},{1,9},{9,1},{257,145}}) {
            int width=size.first,height=size.second;
            for(int pattern=0;pattern<4;++pattern) {
                std::vector<Pixel> pixels(width*height),alpha(width*height);
                for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
                    float value=.45f+.12f*std::sin(x*.9f)+.1f*std::cos(y*.7f);
                    if(pattern==1)value=0;if(pattern==2)value=1;
                    if(pattern==3)value=(x%5==0)?1.4f:-.05f;
                    pixels[y*width+x]={value,value,value,.9f};
                    if(pattern==0) {pixels[y*width+x][1]=.3f+.1f*std::sin((x+y)*.6f);pixels[y*width+x][2]=.6f+.1f*std::cos(x*.4f);}
                    alpha[y*width+x]={.8f,.2f,.1f,static_cast<float>((x+3*y)%17)/16};
                }
                D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=1;
                desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                ComPtr<ID3D11Texture2D> input,original,output,staging;
                D3D11_SUBRESOURCE_DATA data{pixels.data(),static_cast<UINT>(width*sizeof(Pixel)),0};
                Require(device->CreateTexture2D(&desc,&data,&input));data.pSysMem=alpha.data();Require(device->CreateTexture2D(&desc,&data,&original));
                desc.BindFlags=D3D11_BIND_RENDER_TARGET;Require(device->CreateTexture2D(&desc,nullptr,&output));
                desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Require(device->CreateTexture2D(&desc,nullptr,&staging));
                ComPtr<ID3D11ShaderResourceView> inputView,originalView;ComPtr<ID3D11RenderTargetView> target;
                Require(device->CreateShaderResourceView(input.Get(),nullptr,&inputView));Require(device->CreateShaderResourceView(original.Get(),nullptr,&originalView));
                Require(device->CreateRenderTargetView(output.Get(),nullptr,&target));
                double lightDifference=0,strongDifference=0;
                for(float strength:{0.f,.2f,1.f}) {
                    immediate->ClearState();RcasConstants constants{strength,width,height};
                    D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=sizeof(constants);buffer.Usage=D3D11_USAGE_IMMUTABLE;buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
                    D3D11_SUBRESOURCE_DATA initial{&constants,0,0};ComPtr<ID3D11Buffer> cb;Require(device->CreateBuffer(&buffer,&initial,&cb));
                    auto rtv=target.Get();immediate->OMSetRenderTargets(1,&rtv,nullptr);
                    D3D11_VIEWPORT viewport{0,0,static_cast<float>(width),static_cast<float>(height),0,1};immediate->RSSetViewports(1,&viewport);
                    immediate->RSSetState(rasterizer.Get());immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    immediate->VSSetShader(vertex.Get(),nullptr,0);immediate->PSSetShader(shader.Get(),nullptr,0);
                    ID3D11ShaderResourceView* sources[]{inputView.Get(),originalView.Get()};immediate->PSSetShaderResources(0,2,sources);
                    auto constant=cb.Get();immediate->PSSetConstantBuffers(0,1,&constant);immediate->Draw(3,0);
                    immediate->ClearState();immediate->CopyResource(staging.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
                    Require(immediate->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
                    double difference=0;
                    for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
                        auto actual=reinterpret_cast<const Pixel*>(static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch)[x];
                        auto expected=Reference(pixels,x,y,width,height,strength);
                        for(int k=0;k<3;++k) {
                            if(!std::isfinite(actual[k]) || std::abs(actual[k]-expected[k])>.0015f) {
                                fprintf(stderr,"RCAS mismatch size=%dx%d pattern=%d strength=%g pixel=%d,%d channel=%d actual=%g expected=%g\n",width,height,pattern,strength,x,y,k,actual[k],expected[k]);
                                throw std::runtime_error("RCAS GPU/reference mismatch or non-finite output");
                            }
                            if(strength==0 && actual[k]!=pixels[y*width+x][k])throw std::runtime_error("Disabled RCAS changed RGB");
                            difference+=std::abs(actual[k]-pixels[y*width+x][k]);
                        }
                        if(actual[3]!=alpha[y*width+x][3])throw std::runtime_error("RCAS changed original alpha");
                    }
                    immediate->Unmap(staging.Get(),0);
                    if(strength==.2f)lightDifference=difference;if(strength==1)strongDifference=difference;
                }
                if(pattern==0 && width>1 && height>1 && (lightDifference<=.001 || strongDifference<=lightDifference*2))throw std::runtime_error("RCAS strength has no sharpening effect");
            }
        }
        printf("PASS: AMD RCAS GPU/reference agreement at 0/0.2/1, exact disabled RGB/original alpha, finite black/white and out-of-range pixels, clamped edges/1-pixel/odd dimensions and stronger response.\n");
        return 0;
    } catch(const std::exception& error) {fprintf(stderr,"RCAS TEST FAILED: %s\n",error.what());return 1;}
}
