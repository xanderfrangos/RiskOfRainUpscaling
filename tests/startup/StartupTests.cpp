#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <filesystem>

extern "C" __declspec(dllimport) int WINAPI UnityMain(HINSTANCE, HINSTANCE, LPWSTR, int);
int wmain(int argc, wchar_t** argv)
{
    if (argc < 4) return 1;
    // Like Patcher.Initialize: load the adjacent native companion, resolve its
    // initializer, and call it before Unity starts creating graphics resources.
    auto nativePath = std::filesystem::path(argv[1]) / L"RiskOfRainUpscaling.Native.dll";
    HMODULE native = LoadLibraryExW(nativePath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!native) return 2;
    using Initialize = int(__cdecl*)(const wchar_t*, const wchar_t*, const wchar_t*);
    using Count = int(__cdecl*)();
    using Enable = void(__cdecl*)(int);
    using SetMultiplier = int(__cdecl*)(int);
    auto initialize = reinterpret_cast<Initialize>(GetProcAddress(native, "RiskOfRainUpscaling_Initialize"));
    auto count = reinterpret_cast<Count>(GetProcAddress(native, "RiskOfRainUpscaling_GetObservedDeviceCount"));
    auto state = reinterpret_cast<Count>(GetProcAddress(native, "RiskOfRainUpscaling_FrameGenerationState"));
    auto multiplier = reinterpret_cast<Count>(GetProcAddress(native, "RiskOfRainUpscaling_FrameGenerationMultiplier"));
    auto setMultiplier = reinterpret_cast<SetMultiplier>(GetProcAddress(native, "RiskOfRainUpscaling_FrameGenerationSetMultiplier"));
    auto enable = reinterpret_cast<Enable>(GetProcAddress(native, "RiskOfRainUpscaling_FrameGenerationSetEnabled"));
    auto logging = reinterpret_cast<Enable>(GetProcAddress(native, "RiskOfRainUpscaling_SetLoggingLevel"));
    if (!logging) return 3;
    logging(4); // Keep detailed startup evidence in the fixture logs.
    if (!initialize || !count || !state || !enable || !multiplier || !setMultiplier) return 3;
    bool late = argc > 4 && !wcscmp(argv[4], L"--late");
    bool deviceOnly = argc > 4 && !wcscmp(argv[4], L"--device-only");
    bool fgOn = argc > 4 && !wcscmp(argv[4], L"--fg-on");
    // Missed hooks latch a requested Frame Generation as failed; one saved Off stays Off (0).
    int missedState = late && argc > 5 && !wcscmp(argv[5], L"--fg-off") ? 0 : -2;
    if (!late && initialize(argv[2], argv[3], L"preloader") <= 0) return 4;
    int expectedMultiplier=fgOn?(argc>5?_wtoi(argv[5]):2):0;
    if(!late && !deviceOnly && multiplier()!=expectedMultiplier)return 11;
    if(!late && !deviceOnly && !fgOn){
        enable(1); // A live enable request cannot create an absent D3D12/FG runtime.
        if(setMultiplier(4)!=-1 || multiplier()!=0 || state()!=0)return 13;
        enable(0);
    }
    if(!late && !deviceOnly && argc>5){
        // Saved changes after graphics initialization cannot alter this session.
        auto config=std::filesystem::path(argv[2]).parent_path()/L"config"/L"xanderfrangos.riskofrainupscaling.cfg";
        WritePrivateProfileStringW(L"Frame Generation",L"Multiplier",L"2",config.c_str());
        WritePrivateProfileStringW(L"Frame Generation",L"Enabled",L"false",config.c_str());
    }
    int result = UnityMain(GetModuleHandleW(nullptr), nullptr, GetCommandLineW(), SW_HIDE);
    if (result != 73) return result;
    if (count() != (late ? 0 : 1)) return 5;
    auto verified = initialize(argv[2], argv[3], L"plugin-awake");
    if (late || deviceOnly) {
        if (verified != -1 || state() != missedState) return 6;
        enable(1); // A user toggle cannot recover missed startup hooks.
        if (state() != missedState || initialize(argv[2], argv[3], L"plugin-awake") != -1) return 7;
        if (late && (initialize(argv[2], argv[3], L"preloader") <= 0 || state() != missedState)) return 10;
    } else {
        // The WARP fixture has no FG SDK; On can fail safely while still selecting
        // the D3D12 bridge. Off must never request/load the FG runtime.
        if (verified != 0 || (!fgOn && state() != 0) || (fgOn && state() == 0)) return 8;
        if (initialize(argv[2], argv[3], L"preloader") != 0 || count() != 1) return 9;
        if(multiplier()!=expectedMultiplier)return 12;
    }
    printf("PASS: preloader startup, prior resolver/device forwarding, UnityMain=%d, observed devices=%d, FG state=%d, multiplier=%d\n", result, count(), state(),multiplier());
    // Keep native code resident, as the preloader does: hook targets remain live.
    return 0;
}
