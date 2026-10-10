#include "Diagnostics/Diagnostics.h"
#include "SceneInputs/SceneInputs.h"
#include <cmath>
#include <cstdio>
#include <vector>

struct Pixel { float r,g,b,a; };
static ComPtr<ID3D11Texture2D> Texture(ID3D11Device* device, const void* pixels=nullptr, DXGI_FORMAT format=DXGI_FORMAT_R32G32B32A32_FLOAT,
    UINT pitch=4*sizeof(Pixel), UINT samples=1)
{
    D3D11_TEXTURE2D_DESC description{};description.Width=description.Height=4;description.MipLevels=description.ArraySize=1;
    description.Format=format;description.SampleDesc.Count=samples;description.BindFlags=D3D11_BIND_RENDER_TARGET|(samples==1?D3D11_BIND_SHADER_RESOURCE:0);
    D3D11_SUBRESOURCE_DATA initial{pixels,pitch,0};
    ComPtr<ID3D11Texture2D> texture;
    if(FAILED(device->CreateTexture2D(&description,pixels?&initial:nullptr,&texture))) return nullptr;
    return texture;
}
template<class T> static std::vector<T> Read(ID3D11Device* device,ID3D11DeviceContext* immediate,ID3D11Texture2D* texture)
{
    D3D11_TEXTURE2D_DESC description{};texture->GetDesc(&description);description.BindFlags=0;description.Usage=D3D11_USAGE_STAGING;description.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if(FAILED(device->CreateTexture2D(&description,nullptr,&staging))) return {};
    immediate->CopyResource(staging.Get(),texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if(FAILED(immediate->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))) return {};
    std::vector<T> result(16);
    for(int y=0;y<4;++y) for(int x=0;x<4;++x) result[y*4+x]=reinterpret_cast<T*>(static_cast<unsigned char*>(mapped.pData)+y*mapped.RowPitch)[x];
    immediate->Unmap(staging.Get(),0);return result;
}
static bool Equal(float first,float second){return std::abs(first-second)<0.0002f;}
// Owned diagnostics targets for one context; previews are 8-bit as in Unity.
struct Targets {
    ComPtr<ID3D11Texture2D> depth,motion,depthPreview,motionPreview,finalColor,sceneColor,sceneBeforeHud;
    Targets(ID3D11Device* device,const std::vector<Pixel>& depths,const std::vector<Pixel>& motions,DXGI_FORMAT raw,DXGI_FORMAT preview) {
        depth=Texture(device,depths.data(),raw);motion=Texture(device,motions.data(),raw);
        depthPreview=Texture(device,nullptr,preview);motionPreview=Texture(device,nullptr,preview);
        finalColor=Texture(device,nullptr,DXGI_FORMAT_R8G8B8A8_TYPELESS);
        sceneColor=Texture(device,motions.data());sceneBeforeHud=Texture(device,depths.data(),DXGI_FORMAT_R32G32B32A32_FLOAT);
    }
    bool Valid() const { return depth&&motion&&depthPreview&&motionPreview&&finalColor&&sceneColor&&sceneBeforeHud; }
    DiagnosticsDescription Description(int reversed) const {
        return {sizeof(DiagnosticsDescription),1,4,4,4,4,.1f,100,10,2,reversed,0,depth.Get(),motion.Get(),depthPreview.Get(),motionPreview.Get(),
            finalColor.Get(),sceneColor.Get(),sceneBeforeHud.Get()};
    }
};
int main()
{
    wchar_t current[32768]{};GetCurrentDirectoryW(32768,current);
    std::wstring root=current;
    RiskOfRainUpscaling_SetLoggingLevel(4);
    RiskOfRainUpscaling_Initialize((root+L"\\build\\test-results").c_str(),(root+L"\\build\\package").c_str(),L"diagnostics-test");
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> immediate;
    if(FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&immediate))) return 1;
    static_assert(sizeof(DiagnosticsDescription)==104,"Diagnostics ABI");
    static_assert(sizeof(CaptureDescription)==72,"Capture ABI");
    std::vector<Pixel> depths(16),motions(16);
    for(int y=0;y<4;++y) for(int x=0;x<4;++x)
    {
        int i=y*4+x;depths[i]={0.1f+0.1f*x+0.1f*y,0,0,1};motions[i]={0.025f*x-0.05f,0.01f*y-0.02f,0,1};
    }
    auto event=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_DiagnosticsGetEvent());
    auto finalCopy=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_DiagnosticsGetFinalCopyEvent());
    // An asymmetric byte pattern proves the final copy flips only rows.
    std::vector<unsigned> bytes(16);for(unsigned i=0;i<16;++i)bytes[i]=0xff000000|(i*0x010307);
    auto hudTarget=Texture(device.Get(),bytes.data(),DXGI_FORMAT_R8G8B8A8_UNORM,16);
    ComPtr<ID3D11RenderTargetView> hudView;if(!hudTarget||FAILED(device->CreateRenderTargetView(hudTarget.Get(),nullptr,&hudView)))return 2;
    auto Flipped=[&](ID3D11Texture2D* texture){auto read=Read<unsigned>(device.Get(),immediate.Get(),texture);bool match=read.size()==16;
        for(int y=0;match&&y<4;++y)for(int x=0;x<4;++x)match&=read[y*4+x]==bytes[(3-y)*4+x];return match;};
    // Previews: depth/motion values for typed/typeless raw inputs, RGBA/BGRA and typeless 8-bit targets.
    for(auto raw : {DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R32G32B32A32_TYPELESS})
    for(auto preview : {DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_TYPELESS,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_TYPELESS,DXGI_FORMAT_R32G32B32A32_FLOAT})
    for(int reversed=0;reversed<2;++reversed)
    {
        Targets targets(device.Get(),depths,motions,raw,preview);if(!targets.Valid())return 3;
        auto description=targets.Description(reversed);
        auto invalid=description;invalid.size=1;if(RiskOfRainUpscaling_DiagnosticsCreate(&invalid))return 4;
        invalid=description;invalid.farPlane=0;if(RiskOfRainUpscaling_DiagnosticsCreate(&invalid))return 5;
        invalid=description;invalid.sceneColor=nullptr;if(RiskOfRainUpscaling_DiagnosticsCreate(&invalid))return 6;
        invalid=description;invalid.inputWidth=8;if(RiskOfRainUpscaling_DiagnosticsCreate(&invalid))return 7;
        auto context=RiskOfRainUpscaling_DiagnosticsCreate(&description);if(!context)return 8;
        auto pointer=hudView.Get();immediate->OMSetRenderTargets(1,&pointer,nullptr);
        D3D11_VIEWPORT viewport{1,2,3,4,0,1};immediate->RSSetViewports(1,&viewport);
        event(DiagnosticsPreviews,context);finalCopy(1,context);
        ComPtr<ID3D11RenderTargetView> restored;immediate->OMGetRenderTargets(1,&restored,nullptr);
        D3D11_VIEWPORT after{};UINT count=1;immediate->RSGetViewports(&count,&after);
        if(restored.Get()!=hudView.Get()||count!=1||after.TopLeftX!=1||after.Width!=3||RiskOfRainUpscaling_DiagnosticsState(context)!=1)return 9;
        if(!Flipped(targets.finalColor.Get()))return 10;
        bool unorm=preview!=DXGI_FORMAT_R32G32B32A32_FLOAT, bgra=preview==DXGI_FORMAT_B8G8R8A8_UNORM||preview==DXGI_FORMAT_B8G8R8A8_TYPELESS;
        auto Channel=[&](ID3D11Texture2D* texture,int pixel,int channel){
            if(!unorm){auto p=Read<Pixel>(device.Get(),immediate.Get(),texture)[pixel];return channel==0?p.r:channel==1?p.g:p.b;}
            auto value=Read<unsigned>(device.Get(),immediate.Get(),texture)[pixel];
            if(bgra&&channel!=1)channel=2-channel;return ((value>>(8*channel))&255)/255.f;};
        float tolerance=unorm?.003f:.0002f;
        for(int i=0;i<16;++i)
        {
            float denominator=reversed?0.1f+depths[i].r*99.9f:100-depths[i].r*99.9f;
            float eye=10/denominator,expectedDepth=std::log2(1+eye)/std::log2(11.0f);
            float length=std::sqrt(motions[i].r*motions[i].r+motions[i].g*motions[i].g);
            if(std::abs(Channel(targets.depthPreview.Get(),i,0)-expectedDepth)>tolerance)return 11;
            if(std::abs(Channel(targets.motionPreview.Get(),i,0)-(0.5f+motions[i].r*2))>tolerance||
               std::abs(Channel(targets.motionPreview.Get(),i,1)-(0.5f+motions[i].g*2))>tolerance||
               std::abs(Channel(targets.motionPreview.Get(),i,2)-length*2)>tolerance)return 12;
        }
        event(DiagnosticsRetire,context);if(RiskOfRainUpscaling_DiagnosticsState(context)!=-1)return 13;RiskOfRainUpscaling_DiagnosticsDestroy(context);
    }
    // A diagnostics failure stays inside its own context: scene inputs keep working.
    {
        Targets targets(device.Get(),depths,motions,DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R8G8B8A8_UNORM);
        auto description=targets.Description(0);
        auto context=RiskOfRainUpscaling_DiagnosticsCreate(&description);if(!context)return 20;
        SceneInputsDescription inputs{sizeof(inputs),4,4,4,targets.depth.Get()};
        auto scene=RiskOfRainUpscaling_SceneInputsCreate(&inputs);if(!scene)return 21;
        auto multisampled=Texture(device.Get(),nullptr,DXGI_FORMAT_R8G8B8A8_UNORM,16,4);
        ComPtr<ID3D11RenderTargetView> msaa;if(!multisampled||FAILED(device->CreateRenderTargetView(multisampled.Get(),nullptr,&msaa)))return 22;
        auto pointer=msaa.Get();immediate->OMSetRenderTargets(1,&pointer,nullptr);finalCopy(1,context);
        if(RiskOfRainUpscaling_DiagnosticsState(context)!=-2||RiskOfRainUpscaling_SceneInputsState(scene)!=0)return 23;
        // A failed context ignores later events and cannot arm.
        pointer=hudView.Get();immediate->OMSetRenderTargets(1,&pointer,nullptr);event(DiagnosticsPreviews,context);finalCopy(2,context);
        if(RiskOfRainUpscaling_DiagnosticsState(context)!=-2)return 24;
        auto callback=reinterpret_cast<void(__stdcall*)(int,void*)>(RiskOfRainUpscaling_SceneInputsGetEvent());
        event(DiagnosticsRetire,context);RiskOfRainUpscaling_DiagnosticsDestroy(context);
        callback(199,scene);RiskOfRainUpscaling_SceneInputsDestroy(scene);
    }
    // Capture: arm validation, wrong frame, freeze on the requested frame, GPU acknowledgement, cancel and retirement.
    for(int scenario=0;scenario<4;++scenario)
    {
        Targets targets(device.Get(),depths,motions,DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R8G8B8A8_UNORM);
        auto description=targets.Description(0);
        auto context=RiskOfRainUpscaling_DiagnosticsCreate(&description);if(!context)return 30;
        if(RiskOfRainUpscaling_CaptureState(context)!=0)return 31;
        ComPtr<ID3D11Texture2D> frozen[7];ID3D11Texture2D* live[7]{targets.sceneColor.Get(),targets.sceneBeforeHud.Get(),targets.depth.Get(),targets.motion.Get(),
            targets.depthPreview.Get(),targets.motionPreview.Get(),targets.finalColor.Get()};
        for(int i=0;i<7;++i){D3D11_TEXTURE2D_DESC d{};live[i]->GetDesc(&d);frozen[i]=Texture(device.Get(),nullptr,d.Format);}
        CaptureDescription request{sizeof(request),1,100,0,frozen[0].Get(),frozen[1].Get(),frozen[2].Get(),frozen[3].Get(),frozen[4].Get(),frozen[5].Get(),frozen[6].Get()};
        auto invalid=request;invalid.sceneColor=targets.sceneColor.Get();if(RiskOfRainUpscaling_CaptureArm(context,&invalid)==0)return 32;
        invalid=request;invalid.depthPreview=frozen[2].Get();if(RiskOfRainUpscaling_CaptureArm(context,&invalid)==0)return 33; // Format mismatch.
        RiskOfRainUpscaling_CaptureCancel(context);
        if(RiskOfRainUpscaling_CaptureArm(context,&request)||RiskOfRainUpscaling_CaptureArm(context,&request)==0)return 34;
        auto pointer=hudView.Get();immediate->OMSetRenderTargets(1,&pointer,nullptr);
        // Earlier frames update the live copies without freezing.
        event(DiagnosticsPreviews,context);finalCopy(99,context);if(RiskOfRainUpscaling_CaptureState(context)!=1)return 35;
        if(scenario==1){finalCopy(101,context);if(RiskOfRainUpscaling_CaptureState(context)!=-1)return 36;}
        else {
            event(DiagnosticsPreviews,context);finalCopy(100,context);if(RiskOfRainUpscaling_CaptureState(context)!=2)return 37;
            finalCopy(100,context);if(RiskOfRainUpscaling_CaptureState(context)!=2)return 38; // No duplicate freeze.
            if(scenario==2){RiskOfRainUpscaling_CaptureCancel(context);event(DiagnosticsCapturePoll,context);if(RiskOfRainUpscaling_CaptureState(context)!=0)return 39;}
            else if(scenario==3){event(DiagnosticsRetire,context);if(RiskOfRainUpscaling_CaptureState(context)!=-1)return 40;RiskOfRainUpscaling_DiagnosticsDestroy(context);continue;}
            else {
                immediate->Flush();
                auto deadline=GetTickCount64()+5000;
                while(RiskOfRainUpscaling_CaptureState(context)==2&&GetTickCount64()<deadline){event(DiagnosticsCapturePoll,context);SwitchToThread();}
                if(RiskOfRainUpscaling_CaptureState(context)!=3)return 41;
                // Mutating live textures cannot alter a completed capture.
                std::vector<Pixel> zeros(16);immediate->UpdateSubresource(targets.sceneColor.Get(),0,nullptr,zeros.data(),4*sizeof(Pixel),0);
                auto color=Read<Pixel>(device.Get(),immediate.Get(),frozen[0].Get()),depth=Read<Pixel>(device.Get(),immediate.Get(),frozen[2].Get()),
                    motion=Read<Pixel>(device.Get(),immediate.Get(),frozen[3].Get());
                for(int i=0;i<16;++i)if(!Equal(color[i].g,motions[i].g)||!Equal(depth[i].r,depths[i].r)||!Equal(motion[i].g,motions[i].g))return 42;
                if(!Flipped(frozen[6].Get()))return 43;
                RiskOfRainUpscaling_CaptureCancel(context);if(RiskOfRainUpscaling_CaptureState(context)!=0)return 44;
                // Re-arming after export works.
                request.unityFrame=200;if(RiskOfRainUpscaling_CaptureArm(context,&request))return 45;
            }
        }
        event(DiagnosticsRetire,context);RiskOfRainUpscaling_DiagnosticsDestroy(context);
    }
    printf("PASS Diagnostics: depth/motion previews across formats, flipped final copy, state restoration, failure isolation from scene inputs, capture arm validation, frame matching, GPU acknowledgement, cancel and retirement, ABI.\n");
    return 0;
}
