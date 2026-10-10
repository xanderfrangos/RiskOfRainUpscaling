#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstring>

static unsigned resolverCalls{}, deviceCalls{};
static auto realLookup = &GetProcAddress;
static FARPROC WINAPI ExistingLookup(HMODULE module, LPCSTR name);
static HRESULT WINAPI ExistingDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels,
    UINT count, UINT sdk, ID3D11Device** device, D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context)
{
    ++deviceCalls;
    auto create = reinterpret_cast<decltype(&D3D11CreateDevice)>(realLookup(GetModuleHandleW(L"d3d11.dll"), "D3D11CreateDevice"));
    return create(adapter, type, software, flags, levels, count, sdk, device, level, context);
}
static FARPROC WINAPI ExistingLookup(HMODULE module, LPCSTR name)
{
    ++resolverCalls;
    if (reinterpret_cast<uintptr_t>(name) > 0xffff && !strcmp(name, "D3D11CreateDevice") && module == GetModuleHandleW(L"d3d11.dll"))
        return reinterpret_cast<FARPROC>(&ExistingDevice);
    return realLookup(module, name);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    // Simulate the pre-existing Unity/overlay resolver from the first game log.
    auto base = reinterpret_cast<unsigned char*>(instance);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; imports->Name; ++imports)
    {
        if (!imports->OriginalFirstThunk) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imports->OriginalFirstThunk);
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imports->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots)
        {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name;
            if (strcmp(name, "GetProcAddress")) continue;
            realLookup = reinterpret_cast<decltype(realLookup)>(slots->u1.Function);
            DWORD previous{}; VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &previous);
            slots->u1.Function = reinterpret_cast<ULONGLONG>(&ExistingLookup);
            DWORD ignored{}; VirtualProtect(&slots->u1.Function, sizeof(void*), previous, &ignored);
        }
    }
    return TRUE;
}

extern "C" __declspec(dllexport) int WINAPI UnityMain(HINSTANCE instance, HINSTANCE, LPWSTR commandLine, int)
{
    auto module = LoadLibraryW(L"d3d11.dll");
    auto create = reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module, "D3D11CreateDevice"));
    ID3D11Device* device{}; ID3D11DeviceContext* context{};
    if (!create || FAILED(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return 21;
    // Our resolver must still forward unrelated names and missing exports.
    auto kernel = GetModuleHandleW(L"kernel32.dll");
    if (GetProcAddress(kernel, "GetTickCount") != realLookup(kernel, "GetTickCount")) return 29;
    if (GetProcAddress(kernel, "RiskOfRainUpscaling_MissingExport") || GetLastError() != ERROR_PROC_NOT_FOUND) return 30;
    if (wcsstr(commandLine, L"--device-only")) {
        context->Release(); device->Release();
        return deviceCalls == 1 ? 73 : 27;
    }
    auto dxgi = LoadLibraryW(L"dxgi.dll");
    auto factoryCreate = reinterpret_cast<decltype(&CreateDXGIFactory1)>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    IDXGIFactory2* factory{};
    if (!factoryCreate || FAILED(factoryCreate(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory)))) return 22;
    HWND window = CreateWindowExW(0, L"STATIC", L"Preloader startup test", WS_OVERLAPPEDWINDOW, 0, 0, 32, 32, nullptr, nullptr, instance, nullptr);
    DXGI_SWAP_CHAIN_DESC1 description{}; description.Width = description.Height = 16;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM; description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; description.BufferCount = 2; description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* chain{};
    if (FAILED(factory->CreateSwapChainForHwnd(device, window, &description, nullptr, nullptr, &chain))) return 23;
    ID3D11Texture2D* buffer{};
    if (FAILED(chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&buffer)))) return 24;
    buffer->Release();
    if (FAILED(chain->ResizeBuffers(0, 32, 32, DXGI_FORMAT_UNKNOWN, 0))) return 25;
    DXGI_PRESENT_PARAMETERS parameters{};
    if (FAILED(chain->Present1(0, DXGI_PRESENT_TEST, &parameters))) return 26;
    if (FAILED(chain->Present1(0, 0, &parameters))) return 28;
    chain->Release(); factory->Release(); context->Release(); device->Release(); DestroyWindow(window);
    return resolverCalls >= 2 && deviceCalls == 1 ? 73 : 27;
}
