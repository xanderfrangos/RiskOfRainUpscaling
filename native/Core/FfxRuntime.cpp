#include "Core/FfxRuntime.h"
#include <wintrust.h>
#include <softpub.h>
#include <tlhelp32.h>
#include <mutex>
#include <set>
#include <stdexcept>

bool VerifyAmdSignature(const wchar_t* path) {
    WINTRUST_FILE_INFO file{}; file.cbStruct=sizeof(file); file.pcwszFilePath=path;
    WINTRUST_DATA data{}; data.cbStruct=sizeof(data); data.dwUIChoice=WTD_UI_NONE; data.fdwRevocationChecks=WTD_REVOKE_NONE;
    data.dwUnionChoice=WTD_CHOICE_FILE; data.pFile=&file; data.dwStateAction=WTD_STATEACTION_VERIFY; data.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG status=WinVerifyTrust(nullptr,&action,&data);
    bool amd=false;
    if(status==ERROR_SUCCESS) {
        auto provider=WTHelperProvDataFromStateData(data.hWVTStateData);
        auto signer=provider?WTHelperGetProvSignerFromChain(provider,0,FALSE,0):nullptr;
        auto certificate=signer?WTHelperGetProvCertFromChain(signer,0):nullptr;
        if(certificate && certificate->pCert) {
            wchar_t name[256]{}; CertGetNameStringW(certificate->pCert,CERT_NAME_SIMPLE_DISPLAY_TYPE,0,nullptr,name,256);
            amd=!wcscmp(name,L"Advanced Micro Devices");
        }
    }
    data.dwStateAction=WTD_STATEACTION_CLOSE; WinVerifyTrust(nullptr,&action,&data);
    if(!amd) Log(ModLogLevel::Warning,"FFX_SIGNATURE_REJECTED path=%ls status=0x%08lx",path,static_cast<unsigned long>(status));
    return amd;
}
const char* FfxResultName(ffxReturnCode_t code) {
    switch(code) {
    case FFX_API_RETURN_OK: return "OK";
    case FFX_API_RETURN_ERROR: return "ERROR";
    case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return "UNKNOWN_DESCTYPE";
    case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "RUNTIME_ERROR";
    case FFX_API_RETURN_NO_PROVIDER: return "NO_PROVIDER";
    case FFX_API_RETURN_ERROR_MEMORY: return "MEMORY";
    case FFX_API_RETURN_ERROR_PARAMETER: return "PARAMETER";
    case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE: return "PROVIDER_NO_SUPPORT_NEW_DESCTYPE";
    default: return "UNKNOWN";
    }
}
static void FfxMessage(uint32_t type, const wchar_t* message) {
    Log(type==FFX_API_MESSAGE_TYPE_ERROR?ModLogLevel::Error:ModLogLevel::Warning,"FFX_MESSAGE type=%u %ls",type,message?message:L"");
}

// Wine's Authenticode chain support is incomplete; under Proton the owned path
// and module audit still apply, and the signature is reported but not required.
static bool RunningUnderWine() {
    auto ntdll=GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll,"wine_get_version");
}
static HMODULE LoadOwned(const std::wstring& path) {
    if(!VerifyAmdSignature(path.c_str())) {
        if(!RunningUnderWine()) throw std::runtime_error("FidelityFX DLL signature invalid");
        Log(ModLogLevel::Warning,"FFX_SIGNATURE_UNVERIFIED path=%ls; Wine detected, owned runtime path accepted",path.c_str());
    }
    auto module=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!module) throw std::runtime_error("Cannot load owned FidelityFX runtime");
    wchar_t actual[32768]{}; GetModuleFileNameW(module,actual,32768);
    if(_wcsicmp(actual,path.c_str())) throw std::runtime_error("FidelityFX runtime resolved to a foreign path");
    return module;
}

static std::mutex ffxMutex;
static auto ffx=new FfxRuntime(); // Process resident; never unloaded.
static auto loadedEffects=new std::set<std::wstring>();
static bool ffxFailed=false;

const FfxRuntime& LoadFfxRuntime(const wchar_t* effect) {
    std::lock_guard<std::mutex> guard(ffxMutex);
    if(ffxFailed) throw std::runtime_error("FidelityFX runtime failed earlier; restart required");
    if(loadedEffects->count(effect)) return *ffx;
    try {
        auto directory=g_runtimeFolder+L"\\FSR";
        // Another component's FidelityFX DLLs would satisfy the loader's by-name lookup.
        HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,GetCurrentProcessId());
        if(snapshot==INVALID_HANDLE_VALUE) throw std::runtime_error("FidelityFX module ownership audit unavailable");
        MODULEENTRY32W item{}; item.dwSize=sizeof(item); bool foreign=false;
        auto prefix=directory+L"\\";
        if(Module32FirstW(snapshot,&item)) do {
            if(!_wcsnicmp(item.szModule,L"amd_fidelityfx",14) && _wcsnicmp(item.szExePath,prefix.c_str(),prefix.size())) {
                Log(ModLogLevel::Warning,"FFX_FOREIGN_RUNTIME path=%ls",item.szExePath); foreign=true;
            }
        } while(Module32NextW(snapshot,&item));
        CloseHandle(snapshot);
        if(foreign) throw std::runtime_error("Another component owns FidelityFX runtime modules");
        LoadOwned(directory+L"\\"+effect);
        if(!ffx->create) {
            auto loader=LoadOwned(directory+L"\\amd_fidelityfx_loader_dx12.dll");
            auto proc=[&](const char* name){auto p=GetProcAddress(loader,name);if(!p)throw std::runtime_error(name);return p;};
            FfxRuntime loaded{};
            loaded.create=reinterpret_cast<PfnFfxCreateContext>(proc("ffxCreateContext"));
            loaded.destroy=reinterpret_cast<PfnFfxDestroyContext>(proc("ffxDestroyContext"));
            loaded.configure=reinterpret_cast<PfnFfxConfigure>(proc("ffxConfigure"));
            loaded.query=reinterpret_cast<PfnFfxQuery>(proc("ffxQuery"));
            loaded.dispatch=reinterpret_cast<PfnFfxDispatch>(proc("ffxDispatch"));
            ffxConfigureDescGlobalDebug1 debug{}; debug.header.type=FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
            debug.fpMessage=&FfxMessage; debug.debugLevel=FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
            loaded.configure(nullptr,&debug.header);
            *ffx=loaded;
        }
        loadedEffects->insert(effect);
        Log("FFX_RUNTIME_READY loader=%ls\\amd_fidelityfx_loader_dx12.dll effect=%ls",directory.c_str(),effect);
        return *ffx;
    } catch(...) { ffxFailed=true; throw; }
}
