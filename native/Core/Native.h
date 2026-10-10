#pragma once
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <string>
#include <atomic>
#include "Core/Logging.h"

template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
__declspec(dllexport) void Log(const char* format, ...);
__declspec(dllexport) void Log(ModLogLevel severity, const char* format, ...);
bool VerifyNvidiaSignature(const wchar_t* path);
// NVIDIA SDKs are loaded only on adapters with this PCI vendor ID.
constexpr UINT NvidiaVendorId = 0x10DE;
extern std::wstring g_logFolder, g_runtimeFolder;

extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_Initialize(const wchar_t*, const wchar_t*, const wchar_t*);
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_GetObservedDeviceCount();
extern "C" __declspec(dllexport) void __cdecl RiskOfRainUpscaling_SetLoggingLevel(int);
