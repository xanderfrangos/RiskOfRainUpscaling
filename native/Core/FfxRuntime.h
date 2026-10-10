#pragma once
#include "Core/Native.h"
#include <api/include/ffx_api.h>

// The process-owned AMD FidelityFX API. Effect DLLs load from runtimes\FSR by
// absolute path before the loader, so the loader resolves our copies by name.
struct FfxRuntime {
    PfnFfxCreateContext create{};
    PfnFfxDestroyContext destroy{};
    PfnFfxConfigure configure{};
    PfnFfxQuery query{};
    PfnFfxDispatch dispatch{};
};
// Loads the loader and the given effect DLL (e.g. L"amd_fidelityfx_framegeneration_dx12.dll")
// once per process; throws std::runtime_error on failure.
const FfxRuntime& LoadFfxRuntime(const wchar_t* effect);
// Authenticode-valid and signed by Advanced Micro Devices.
bool VerifyAmdSignature(const wchar_t* path);
const char* FfxResultName(ffxReturnCode_t);
