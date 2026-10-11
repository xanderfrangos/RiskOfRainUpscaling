#include "Presentation/StreamlinePresentation.h"
#include <map>
#include <set>
#include <stdexcept>

std::recursive_mutex streamlineMutex;
struct PresentationRuntime {
    ID3D11Device* device{}; // The process-owned DLAA runtime retains this device.
    PFun_slUpgradeInterface* upgrade{};
    bool failed{};
    unsigned presented{};
    // Keys are native IUnknown identities, held alive by the proxy's native reference.
    std::map<IUnknown*, ComPtr<IDXGISwapChain>> proxies;
    std::set<IUnknown*> observed;
};
static auto presentation = new PresentationRuntime();

void EnableStreamlinePresentation(ID3D11Device* device, PFun_slUpgradeInterface* upgrade) {
    std::lock_guard<std::recursive_mutex> guard(streamlineMutex);
    presentation->device=device; presentation->upgrade=upgrade;
}
bool StreamlinePresentationHealthy() {
    std::lock_guard<std::recursive_mutex> guard(streamlineMutex);
    return !presentation->failed;
}
void FailStreamlinePresentation() {
    std::lock_guard<std::recursive_mutex> guard(streamlineMutex);
    presentation->failed=true;
}
void TrackStreamlineSwapChain(IDXGISwapChain* chain) {
    std::lock_guard<std::recursive_mutex> guard(streamlineMutex);
    ComPtr<IUnknown> identity;
    if(SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&identity))))presentation->observed.insert(identity.Get());
}
bool StreamlinePresentationObserved() {
    std::lock_guard<std::recursive_mutex> guard(streamlineMutex);
    return !presentation->observed.empty();
}
unsigned StreamlinePresentedFrames() {
    std::lock_guard<std::recursive_mutex> guard(streamlineMutex);
    return presentation->presented;
}
bool RouteThroughStreamline(IDXGISwapChain* chain, const std::function<void(IDXGISwapChain*)>& call) {
    std::lock_guard<std::recursive_mutex> guard(streamlineMutex);
    if (!presentation->upgrade) return false;
    ComPtr<ID3D11Device> device;
    if (FAILED(chain->GetDevice(IID_PPV_ARGS(&device))) || device.Get()!=presentation->device) return false;
    ComPtr<IUnknown> identity;
    if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&identity)))) return false;
    // Shared DXGI vtables also intercept chains made by overlays. Only route
    // chains observed through Unity/game graphics creation, on our owned device.
    if(!presentation->observed.count(identity.Get()))return false;
    auto found=presentation->proxies.find(identity.Get());
    if(found==presentation->proxies.end()) {
        if(presentation->failed) return false;
        // Unity already created its native chain before the first DLAA activation.
        // Upgrade a private interface at this serialized late-binding boundary.
        // The SDK proxy acquires its own native reference; Unity keeps its identity.
        void* upgraded=chain;
        auto result=presentation->upgrade(&upgraded);
        if(result!=sl::Result::eOk || upgraded==chain) {
            presentation->failed=true;
            Log(ModLogLevel::Error, "STREAMLINE_PRESENT_FAILED slUpgradeInterface=%d proxyCreated=%d; DLAA will fall back",static_cast<int>(result),upgraded!=chain);
            return false;
        }
        ComPtr<IDXGISwapChain> proxy; proxy.Attach(static_cast<IDXGISwapChain*>(upgraded));
        found=presentation->proxies.emplace(identity.Get(),std::move(proxy)).first;
        Log("STREAMLINE_PRESENT_BOUND native=%p proxy=%p; D3D11 SDK manual hooks; native Unity identity retained",chain,found->second.Get());
    }
    // Never retry a call after entering the SDK: it may already have presented.
    call(found->second.Get());
    return true;
}
ULONG ReleaseStreamlineSwapChain(IUnknown* key, ULONG remaining) {
    std::lock_guard<std::recursive_mutex> guard(streamlineMutex);
    if(!remaining) {presentation->observed.erase(key);return 0;}
    if(remaining!=1)return remaining;
    auto found=presentation->proxies.find(key);
    if(found==presentation->proxies.end()) return remaining;
    auto proxy=std::move(found->second); presentation->proxies.erase(found);
    presentation->observed.erase(key);
    Log("STREAMLINE_PRESENT_UNBOUND native=%p; releasing final proxy-owned reference",key);
    proxy.Reset(); // Native Release re-enters the guarded hook and destroys the chain.
    return 0;
}
void RecordStreamlinePresent(UINT flags, bool nativeCalled, HRESULT result) {
    std::lock_guard<std::recursive_mutex> guard(streamlineMutex);
    if(flags & DXGI_PRESENT_TEST) return;
    if(!nativeCalled || FAILED(result)) {
        Log("STREAMLINE_PRESENT_RESULT nativeCalled=%d HRESULT=0x%08lx",nativeCalled,result);
        if(!nativeCalled) presentation->failed=true;
        return;
    }
    unsigned count=++presentation->presented;
    if(count<=4 || count%300==0)
        Log("STREAMLINE_PRESENT_SERVICED count=%u originalCalls=1 HRESULT=0x%08lx; SDK before/after Present hooks executed",count,result);
}
