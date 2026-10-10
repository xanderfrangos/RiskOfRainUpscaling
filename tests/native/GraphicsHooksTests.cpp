#include "Core/Native.h"
#include <cstdio>
#include <cstring>

int main(int argc, char** argv)
{
    bool warp = argc < 2 || !strcmp(argv[1], "--warp");
    wchar_t current[32768]{}; GetCurrentDirectoryW(32768, current);
    auto root = std::wstring(current);
    RiskOfRainUpscaling_SetLoggingLevel(4); // Detailed fixture evidence.
    RiskOfRainUpscaling_Initialize((root + L"\\build\\test-results").c_str(), (root + L"\\build\\package\\BepInEx\\plugins\\RiskOfRainUpscaling\\runtimes").c_str(), L"standalone-test");
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return 1;
    // Exercise pass-through startup, factory, swap-chain and buffer hooks on a hidden window.
    HWND window = CreateWindowExW(0, L"STATIC", L"RiskOfRainUpscaling native test", WS_OVERLAPPEDWINDOW, 0, 0, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) return 5;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 6;
    DXGI_SWAP_CHAIN_DESC1 chainDesc{}; chainDesc.Width = chainDesc.Height = 16;
    chainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; chainDesc.SampleDesc.Count = 1;
    chainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; chainDesc.BufferCount = 2; chainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain;
    if (FAILED(factory->CreateSwapChainForHwnd(device.Get(), window, &chainDesc, nullptr, nullptr, &chain))) return 7;
    ComPtr<ID3D11Texture2D> backbuffer;
    if (FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) return 8;
    backbuffer.Reset();
    if (FAILED(chain->ResizeBuffers(0, 32, 32, DXGI_FORMAT_UNKNOWN, 0))) return 9;
    DXGI_PRESENT_PARAMETERS present{};
    if (FAILED(chain->Present1(0, DXGI_PRESENT_TEST, &present))) return 10;
    chain.Reset(); factory.Reset(); DestroyWindow(window);
    if (RiskOfRainUpscaling_GetObservedDeviceCount() != 1) return 11;
    printf("Native %s startup hooks passed; log=build/test-results/native.log\n", warp ? "WARP" : "hardware");
    return 0;
}
