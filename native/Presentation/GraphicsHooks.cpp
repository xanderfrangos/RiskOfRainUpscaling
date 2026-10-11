#include "Presentation/GraphicsHooks.h"
#include "Presentation/StreamlinePresentation.h"
#include "FrameGeneration/FrameBridge.h"
#include "FrameGeneration/FrameGeneration.h"
#include "Diagnostics/Profiler.h"
#include <mutex>
#include <map>
#include <cstring>

static std::atomic<int> creations{0}, presents{0}, buffers{0}, swapchains{0};
static std::mutex patchMutex;
static std::map<void**, void*> originals;
int ObservedDeviceCount() { return creations.load(); }
static void WarnHooksMissed()
{
    static std::atomic<bool> warned{false};
    if (warned.exchange(true)) return;
    Log(ModLogLevel::Warning, "WARNING EARLY_HOOKS_MISSED observedDevices=%d observedSwapchains=%d; Frame Generation stays OFF and DLSS is unavailable (Auto upscaling uses FSR) until restart. Check the BepInEx preloader and startup tracing configuration.", creations.load(), swapchains.load());
    FrameGenerationFail("Early graphics hooks missed; restart with the preloader installed and startup tracing enabled");
}
bool VerifyGraphicsHooks()
{
    if (creations.load() && swapchains.load()) return true;
    WarnHooksMissed();
    return false;
}
using Resolver = FARPROC (WINAPI*)(HMODULE, LPCSTR);
static std::atomic<Resolver> gameResolver{nullptr}, unityResolver{nullptr};
static std::mutex resolverMutex;
static std::map<std::string, FARPROC> graphicsTargets;

static void LogAddress(const char* label, void* pointer)
{
    MEMORY_BASIC_INFORMATION memory{}; wchar_t path[32768]{};
    if (VirtualQuery(pointer, &memory, sizeof(memory)) && memory.Type == MEM_IMAGE)
        GetModuleFileNameW(static_cast<HMODULE>(memory.AllocationBase), path, 32768);
    Log(ModLogLevel::Debug, "%s address=%p owner=%ls protection=0x%lx", label, pointer, path[0] ? path : L"<unknown/private memory>", memory.Protect);
}

// A previous resolver may return an overlay's graphics function. Forward to that
// exact address rather than bypassing it with our own D3D11/DXGI imports.
template<class T> static T GraphicsTarget(const char* name, T fallback)
{
    std::lock_guard<std::mutex> guard(resolverMutex);
    auto found = graphicsTargets.find(name);
    return found == graphicsTargets.end() ? fallback : reinterpret_cast<T>(found->second);
}

static bool PatchSlot(void** slot, void* hook)
{
    std::lock_guard<std::mutex> guard(patchMutex);
    if (*slot == hook) return false;
    DWORD old{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    originals[slot] = *slot;
    InterlockedExchangePointer(slot, hook);
    DWORD ignored{}; VirtualProtect(slot, sizeof(void*), old, &ignored);
    return true;
}

template<class T> static T Original(void* object, int index)
{
    std::lock_guard<std::mutex> guard(patchMutex);
    auto table = *reinterpret_cast<void***>(object);
    return reinterpret_cast<T>(originals.at(table + index));
}

static thread_local bool redirecting=false;
static thread_local unsigned nativePresentCalls=0;
static thread_local HRESULT nativePresentResult=E_FAIL;
struct RedirectGuard {
    bool previous=redirecting;
    RedirectGuard(){redirecting=true;}
    ~RedirectGuard(){redirecting=previous;}
};
static bool RouteChain(IDXGISwapChain* chain,const std::function<void(IDXGISwapChain*)>& call) {
    if(redirecting)return false;
    RedirectGuard guard;
    return RouteThroughStreamline(chain,call);
}
static HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* chain, UINT interval, UINT flags)
{
    auto original=Original<HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT)>(chain,8);
    if(redirecting) {++nativePresentCalls; return nativePresentResult=original(chain,interval,flags);}
    if(!(flags & DXGI_PRESENT_TEST)) ProfilerPresentChain(chain);
    if (presents.fetch_add(1) == 0) Log(ModLogLevel::Debug, "FIRST_PRESENT chain=%p interval=%u flags=0x%x", chain, interval, flags);
    HRESULT result=E_FAIL; nativePresentCalls=0;
    bool routed=RouteChain(chain,[&](IDXGISwapChain* proxy){result=proxy->Present(interval,flags);});
    if(!routed)result=original(chain,interval,flags);
    else {
        // SDK after-hooks can replace an occlusion/error result with S_OK.
        // Preserve the actual native result for Unity.
        if(nativePresentCalls && (FAILED(nativePresentResult) || SUCCEEDED(result)))result=nativePresentResult;
        RecordStreamlinePresent(flags,nativePresentCalls==1,result);
    }
    return result;
}
static HRESULT STDMETHODCALLTYPE Present1(IDXGISwapChain1* chain, UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters)
{
    auto original=Original<HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*)>(chain,22);
    if(redirecting) {++nativePresentCalls;return nativePresentResult=original(chain,interval,flags,parameters);}
    if(!(flags & DXGI_PRESENT_TEST)) ProfilerPresentChain(chain);
    if (presents.fetch_add(1) == 0) Log(ModLogLevel::Debug, "FIRST_PRESENT1 chain=%p interval=%u flags=0x%x", chain, interval, flags);
    HRESULT result=E_NOINTERFACE;nativePresentCalls=0;
    bool routed=RouteChain(chain,[&](IDXGISwapChain* proxy){
        ComPtr<IDXGISwapChain1> proxy1;
        if(SUCCEEDED(proxy->QueryInterface(IID_PPV_ARGS(&proxy1))))result=proxy1->Present1(interval,flags,parameters);
    });
    if(!routed)result=original(chain,interval,flags,parameters);
    else {if(nativePresentCalls && (FAILED(nativePresentResult) || SUCCEEDED(result)))result=nativePresentResult;RecordStreamlinePresent(flags,nativePresentCalls==1,result);}
    return result;
}
static void LogInterface(const char* operation, void* chain, REFIID iid, HRESULT result)
{
    Log("%s chain=%p iid=%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x HRESULT=0x%08lx", operation, chain, iid.Data1, iid.Data2, iid.Data3,
        iid.Data4[0], iid.Data4[1], iid.Data4[2], iid.Data4[3], iid.Data4[4], iid.Data4[5], iid.Data4[6], iid.Data4[7], result);
}
static HRESULT STDMETHODCALLTYPE GetBuffer(IDXGISwapChain* chain, UINT index, REFIID iid, void** output)
{
    HRESULT result=E_FAIL;
    if(!RouteChain(chain,[&](IDXGISwapChain* proxy){result=proxy->GetBuffer(index,iid,output);}))
        result=Original<HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, REFIID, void**)>(chain,9)(chain,index,iid,output);
    if (buffers.fetch_add(1) < 8) { Log(ModLogLevel::Debug, "GET_BUFFER index=%u", index); LogInterface("GET_BUFFER_INTERFACE", chain, iid, result); }
    return result;
}
static HRESULT STDMETHODCALLTYPE ResizeBuffers(IDXGISwapChain* chain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
    if(!redirecting)Log("RESIZE_BUFFERS chain=%p count=%u size=%ux%u format=%u flags=0x%x", chain, count, width, height, format, flags);
    HRESULT result=E_FAIL;
    if(!RouteChain(chain,[&](IDXGISwapChain* proxy){result=proxy->ResizeBuffers(count,width,height,format,flags);}))
        result=Original<HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT)>(chain,13)(chain,count,width,height,format,flags);
    return result;
}
static HRESULT STDMETHODCALLTYPE SetFullscreenState(IDXGISwapChain* chain,BOOL fullscreen,IDXGIOutput* output) {
    HRESULT result=E_FAIL;
    if(!RouteChain(chain,[&](IDXGISwapChain* proxy){result=proxy->SetFullscreenState(fullscreen,output);}))
        result=Original<HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,BOOL,IDXGIOutput*)>(chain,10)(chain,fullscreen,output);
    return result;
}
static UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex(IDXGISwapChain3* chain) {
    UINT result=0;
    if(!RouteChain(chain,[&](IDXGISwapChain* proxy){ComPtr<IDXGISwapChain3> proxy3;
        if(SUCCEEDED(proxy->QueryInterface(IID_PPV_ARGS(&proxy3))))result=proxy3->GetCurrentBackBufferIndex();}))
        result=Original<UINT(STDMETHODCALLTYPE*)(IDXGISwapChain3*)>(chain,36)(chain);
    return result;
}
static HRESULT STDMETHODCALLTYPE ResizeBuffers1(IDXGISwapChain3* chain,UINT count,UINT width,UINT height,DXGI_FORMAT format,UINT flags,const UINT* nodes,IUnknown* const* queues) {
    HRESULT result=E_NOINTERFACE;
    if(!RouteChain(chain,[&](IDXGISwapChain* proxy){ComPtr<IDXGISwapChain3> proxy3;
        if(SUCCEEDED(proxy->QueryInterface(IID_PPV_ARGS(&proxy3))))result=proxy3->ResizeBuffers1(count,width,height,format,flags,nodes,queues);}))
        result=Original<HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*,UINT,UINT,UINT,DXGI_FORMAT,UINT,const UINT*,IUnknown* const*)>(chain,39)(chain,count,width,height,format,flags,nodes,queues);
    return result;
}
static ULONG STDMETHODCALLTYPE ReleaseChain(IDXGISwapChain* chain) {
    auto original=Original<ULONG(STDMETHODCALLTYPE*)(IDXGISwapChain*)>(chain,2);
    if(redirecting)return original(chain);
    std::lock_guard<std::recursive_mutex> guard(streamlineMutex);
    RedirectGuard redirect;
    // Resolve identity while the caller's reference still guarantees lifetime.
    ComPtr<IUnknown> identity;
    chain->QueryInterface(IID_PPV_ARGS(&identity));
    auto key=identity.Get();identity.Reset();
    auto remaining=original(chain);
    return ReleaseStreamlineSwapChain(key,remaining);
}
void ObserveChain(IDXGISwapChain* chain)
{
    if (!chain) return;
    ++swapchains;
    DXGI_SWAP_CHAIN_DESC desc{}; chain->GetDesc(&desc);
    Log("SWAPCHAIN_CREATED chain=%p HWND=%p count=%u size=%ux%u effect=%u; early replacement hook point observed", chain, desc.OutputWindow,
        desc.BufferCount, desc.BufferDesc.Width, desc.BufferDesc.Height, desc.SwapEffect);
    auto observeTable = [](void* pointer, int version)
    {
        auto table = *reinterpret_cast<void***>(pointer);
        PatchSlot(table + 2, reinterpret_cast<void*>(&ReleaseChain));
        PatchSlot(table + 8, reinterpret_cast<void*>(&Present));
        PatchSlot(table + 9, reinterpret_cast<void*>(&GetBuffer));
        PatchSlot(table + 10, reinterpret_cast<void*>(&SetFullscreenState));
        PatchSlot(table + 13, reinterpret_cast<void*>(&ResizeBuffers));
        if (version>=1) PatchSlot(table + 22, reinterpret_cast<void*>(&Present1));
        if (version>=3) {PatchSlot(table + 36,reinterpret_cast<void*>(&GetCurrentBackBufferIndex));PatchSlot(table + 39,reinterpret_cast<void*>(&ResizeBuffers1));}
    };
    observeTable(chain, 0);
    ComPtr<IDXGISwapChain1> chain1;
    if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain1)))) observeTable(chain1.Get(), 1);
    ComPtr<IDXGISwapChain3> chain3;
    if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain3)))) observeTable(chain3.Get(),3);
    TrackStreamlineSwapChain(chain);
}
static HRESULT STDMETHODCALLTYPE CreateSwapChain(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** output)
{
    Log(ModLogLevel::Debug, "BEFORE_CREATE_SWAPCHAIN factory=%p device=%p", factory, device);
    if(desc && device && output && desc->Windowed && FrameBridgeWindowEligible(desc->OutputWindow)){
        ComPtr<IDXGIFactory2> factory2;ComPtr<ID3D11Device> device11;
        if(SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory2))) && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device11)))){
            DXGI_SWAP_CHAIN_DESC1 d{};d.Width=desc->BufferDesc.Width;d.Height=desc->BufferDesc.Height;d.Format=desc->BufferDesc.Format;
            d.SampleDesc=desc->SampleDesc;d.BufferUsage=desc->BufferUsage;d.BufferCount=desc->BufferCount;d.SwapEffect=desc->SwapEffect;d.Flags=desc->Flags;
            d.Scaling=DXGI_SCALING_STRETCH;d.AlphaMode=DXGI_ALPHA_MODE_UNSPECIFIED;
            ComPtr<IDXGISwapChain1> bridge;
            if(SUCCEEDED(CreateFrameBridge(factory2.Get(),device11.Get(),desc->OutputWindow,&d,nullptr,nullptr,&bridge))){
                auto hr=bridge->QueryInterface(IID_PPV_ARGS(output));if(SUCCEEDED(hr))ObserveChain(*output);return hr;
            }
            Log(ModLogLevel::Error, "FRAME_BRIDGE_FALLBACK legacy descriptor unsupported or bridge creation failed; native display selected");
        }
    }
    auto hr = Original<HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**)>(factory, 10)(factory, device, desc, output);
    if (SUCCEEDED(hr) && output && !BuildingFrameBridge()) ObserveChain(*output);
    return hr;
}
static HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IDXGIFactory2* factory, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* restrictOutput, IDXGISwapChain1** output)
{
    Log(ModLogLevel::Debug, "BEFORE_CREATE_SWAPCHAIN_FOR_HWND factory=%p device=%p HWND=%p", factory, device, hwnd);
    if(device && output && FrameBridgeWindowEligible(hwnd)){
        ComPtr<ID3D11Device> device11;
        if(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device11)))){
            auto hr=CreateFrameBridge(factory,device11.Get(),hwnd,desc,fullscreen,restrictOutput,output);
            if(SUCCEEDED(hr)){ObserveChain(*output);return hr;}
            Log(ModLogLevel::Warning, "FRAME_BRIDGE_FALLBACK HRESULT=0x%08lx; native display selected",hr);
        }
    }
    auto hr = Original<HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**)>(factory, 15)(factory, device, hwnd, desc, fullscreen, restrictOutput, output);
    if (SUCCEEDED(hr) && output && !BuildingFrameBridge()) ObserveChain(*output);
    return hr;
}
static void ObserveFactory(IUnknown* unknown)
{
    if (!unknown) return;
    ComPtr<IDXGIFactory> factory;
    if (SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&factory))))
    {
        auto table = *reinterpret_cast<void***>(factory.Get()); PatchSlot(table + 10, reinterpret_cast<void*>(&CreateSwapChain));
    }
    ComPtr<IDXGIFactory2> factory2;
    if (SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&factory2))))
    {
        auto table = *reinterpret_cast<void***>(factory2.Get());
        PatchSlot(table + 10, reinterpret_cast<void*>(&CreateSwapChain)); PatchSlot(table + 15, reinterpret_cast<void*>(&CreateSwapChainForHwnd));
    }
}

static HRESULT WINAPI Device(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT count,
    UINT sdk, ID3D11Device** device, D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context)
{
    ++creations; Log(ModLogLevel::Debug, "BEFORE_D3D11_CREATE_DEVICE adapter=%p flags=0x%x", adapter, flags);
    auto hr = GraphicsTarget("D3D11CreateDevice", &D3D11CreateDevice)(adapter, driver, software, flags, levels, count, sdk, device, level, context);
    Log(ModLogLevel::Debug, "D3D11_CREATE_DEVICE HRESULT=0x%08lx device=%p", hr, SUCCEEDED(hr) && device ? *device : nullptr); return hr;
}
static HRESULT WINAPI DeviceAndChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels,
    UINT count, UINT sdk, const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** chain, ID3D11Device** device, D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context)
{
    ++creations; Log(ModLogLevel::Debug, "BEFORE_D3D11_CREATE_DEVICE_AND_SWAPCHAIN adapter=%p", adapter);
    if(FrameBridgeRequested())Log(ModLogLevel::Warning, "FRAME_BRIDGE_FALLBACK combined device/chain creation retains Native; the presentation bridge needs Unity's separate factory path");
    auto hr = GraphicsTarget("D3D11CreateDeviceAndSwapChain", &D3D11CreateDeviceAndSwapChain)(adapter, driver, software, flags, levels, count, sdk, desc, chain, device, level, context);
    Log(ModLogLevel::Debug, "D3D11_CREATE_DEVICE_AND_SWAPCHAIN HRESULT=0x%08lx", hr);
    if (SUCCEEDED(hr) && chain) ObserveChain(*chain); return hr;
}
static HRESULT WINAPI Factory(REFIID iid, void** output)
{
    Log(ModLogLevel::Debug, "BEFORE_CREATE_DXGI_FACTORY");
    auto hr = GraphicsTarget("CreateDXGIFactory", &CreateDXGIFactory)(iid, output); if (SUCCEEDED(hr) && output) ObserveFactory(static_cast<IUnknown*>(*output)); return hr;
}
static HRESULT WINAPI Factory1(REFIID iid, void** output)
{
    Log(ModLogLevel::Debug, "BEFORE_CREATE_DXGI_FACTORY1");
    auto hr = GraphicsTarget("CreateDXGIFactory1", &CreateDXGIFactory1)(iid, output); if (SUCCEEDED(hr) && output) ObserveFactory(static_cast<IUnknown*>(*output)); return hr;
}
static HRESULT WINAPI Factory2(UINT flags, REFIID iid, void** output)
{
    Log(ModLogLevel::Debug, "BEFORE_CREATE_DXGI_FACTORY2 flags=0x%x", flags);
    auto hr = GraphicsTarget("CreateDXGIFactory2", &CreateDXGIFactory2)(flags, iid, output); if (SUCCEEDED(hr) && output) ObserveFactory(static_cast<IUnknown*>(*output)); return hr;
}
static void* Replacement(const char* name)
{
    if (!strcmp(name, "D3D11CreateDevice")) return reinterpret_cast<void*>(&Device);
    if (!strcmp(name, "D3D11CreateDeviceAndSwapChain")) return reinterpret_cast<void*>(&DeviceAndChain);
    if (!strcmp(name, "CreateDXGIFactory")) return reinterpret_cast<void*>(&Factory);
    if (!strcmp(name, "CreateDXGIFactory1")) return reinterpret_cast<void*>(&Factory1);
    if (!strcmp(name, "CreateDXGIFactory2")) return reinterpret_cast<void*>(&Factory2);
    return nullptr;
}
static bool RememberGraphicsTarget(const char* name, FARPROC actual)
{
    std::lock_guard<std::mutex> guard(resolverMutex);
    auto found = graphicsTargets.find(name);
    if (found != graphicsTargets.end()) return found->second == actual;
    graphicsTargets.emplace(name, actual);
    return true;
}
static FARPROC Lookup(Resolver resolver, HMODULE module, LPCSTR name)
{
    auto actual = resolver(module, name);
    DWORD error = GetLastError();
    if (reinterpret_cast<uintptr_t>(name) <= 0xffff || !actual) return actual;
    if (module != GetModuleHandleW(L"d3d11.dll") && module != GetModuleHandleW(L"dxgi.dll")) { SetLastError(error); return actual; }
    auto hook = Replacement(name);
    if (hook && actual != reinterpret_cast<FARPROC>(hook))
    {
        if (RememberGraphicsTarget(name, actual))
        {
            Log(ModLogLevel::Debug, "DYNAMIC_GRAPHICS_LOOKUP %s; preserving resolved target", name);
            LogAddress("GRAPHICS_LOOKUP_TARGET", reinterpret_cast<void*>(actual));
            SetLastError(error); return reinterpret_cast<FARPROC>(hook);
        }
        Log(ModLogLevel::Debug, "GRAPHICS_LOOKUP_SKIPPED %s multiple resolved targets; preserving caller result", name);
    }
    SetLastError(error);
    return actual;
}
static FARPROC WINAPI LookupGame(HMODULE module, LPCSTR name) { return Lookup(gameResolver.load(), module, name); }
static FARPROC WINAPI LookupUnity(HMODULE module, LPCSTR name) { return Lookup(unityResolver.load(), module, name); }

static int PatchImports(HMODULE module, const char* label, std::atomic<Resolver>& resolver, void* lookup)
{
    if (!module) { Log(ModLogLevel::Debug, "STARTUP_COVERAGE %s not loaded; hook point unproven", label); return 0; }
    auto base = reinterpret_cast<unsigned char*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return 0;
    int patched = 0;
    auto imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; imports->Name; ++imports)
    {
        if (!imports->OriginalFirstThunk) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imports->OriginalFirstThunk);
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imports->FirstThunk);
        auto importedDll = reinterpret_cast<const char*>(base + imports->Name);
        for (; names->u1.AddressOfData; ++names, ++slots)
        {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name;
            bool graphics = !_stricmp(importedDll, "d3d11.dll") || !_stricmp(importedDll, "dxgi.dll");
            void* hook = graphics ? Replacement(name) : nullptr;
            bool isResolver = !strcmp(name, "GetProcAddress");
            if (isResolver) hook = lookup;
            if (hook)
            {
                auto slot = reinterpret_cast<void**>(&slots->u1.Function);
                auto source = GetModuleHandleA(importedDll);
                auto expected = source ? reinterpret_cast<void*>(GetProcAddress(source, name)) : nullptr;
                if (*slot == hook) continue;
                LogAddress("IMPORT_CURRENT_TARGET", *slot);
                LogAddress("IMPORT_WINDOWS_TARGET", expected);
                if (isResolver)
                {
                    // Once installed, do not re-chain a later hook that may already
                    // call us: that could create a cycle. Report lost coverage.
                    if (resolver.load()) { Log(ModLogLevel::Debug, "IMPORT_HOOK_SKIPPED %s!%s replaced after our installation", label, name); continue; }
                    MEMORY_BASIC_INFORMATION memory{};
                    if (!VirtualQuery(*slot, &memory, sizeof(memory)) || memory.State != MEM_COMMIT ||
                        !(memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) || (memory.Protect & PAGE_GUARD))
                    { Log(ModLogLevel::Debug, "IMPORT_HOOK_SKIPPED %s!%s target is not executable", label, name); continue; }
                    resolver.store(reinterpret_cast<Resolver>(*slot));
                    if (PatchSlot(slot, hook)) { ++patched; Log(ModLogLevel::Debug, "IMPORT_RESOLVER_CHAIN %s!%s priorTargetPreserved=true", label, name); }
                    else resolver.store(nullptr);
                    continue;
                }
                // Preserve prior import hooks from overlays/other mods. If they hide the
                // startup calls, report incomplete coverage instead of bypassing them.
                if (!expected || *slot != expected) { Log(ModLogLevel::Debug, "IMPORT_HOOK_SKIPPED %s!%s existing hook/unknown owner", label, name); continue; }
                if (!RememberGraphicsTarget(name, reinterpret_cast<FARPROC>(*slot))) { Log(ModLogLevel::Debug, "IMPORT_HOOK_SKIPPED %s!%s conflicting graphics target", label, name); continue; }
                if (PatchSlot(slot, hook)) { ++patched; Log(ModLogLevel::Debug, "IMPORT_HOOK %s!%s from %s", label, name, importedDll); }
            }
        }
    }
    Log(ModLogLevel::Debug, "STARTUP_COVERAGE %s newImportHooks=%d observedDevices=%d observedSwapchains=%d; absence of events does not prove early timing", label, patched, creations.load(), swapchains.load());
    return patched;
}
int InstallGraphicsHooks()
{
    auto patched = PatchImports(GetModuleHandleW(nullptr), "game.exe", gameResolver, reinterpret_cast<void*>(&LookupGame)) +
        PatchImports(GetModuleHandleW(L"UnityPlayer.dll"), "UnityPlayer.dll", unityResolver, reinterpret_cast<void*>(&LookupUnity));
    if (!patched && !gameResolver.load() && !unityResolver.load()) WarnHooksMissed();
    return patched;
}
