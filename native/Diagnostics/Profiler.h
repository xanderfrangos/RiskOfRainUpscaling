#pragma once
#include "Core/Native.h"

// Cost profiler ([Diagnostics] Profiling). D3D11 GPU timestamps of each
// upscaling / Frame Generation step on Unity's immediate context, grouped into
// frames closed at Present, plus render/present-thread CPU times and D3D12 samples
// from the presentation bridge. Every two seconds the averages are written to
// profile.log. All calls are no-ops while profiling is off.

// Stable id for a label; the same name always returns the same id.
int ProfilerLabel(const char* name);
bool ProfilerEnabled();
// GPU timestamp brackets and instants on the bound device's immediate context.
// Call from the render thread only.
void ProfilerBegin(int label);
void ProfilerEnd(int label);
void ProfilerMark(int label);
// Closes the current GPU frame (call before Present on the render thread) and
// resolves finished frames without waiting. Binds the device on first use.
// bridge: the D3D12 presentation bridge; hooked swap-chain Presents are then ignored.
void ProfilerPresent(ID3D11Device*, bool bridge = false);
void ProfilerPresentChain(IDXGISwapChain*);
// Samples measured elsewhere: CPU milliseconds, or GPU milliseconds from D3D12.
void ProfilerCpu(int label, double milliseconds);
void ProfilerGpu(int label, double milliseconds);
// True while the profiler issues its own query; HUD opacity capture ignores it.
bool ProfilerIssuingQuery();

// GPU bracket plus CPU time for a native step on the render thread.
struct ProfileScope {
    int label; LARGE_INTEGER started{};
    explicit ProfileScope(int id);
    ~ProfileScope();
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;
};
// CPU-only timer (present thread, waits).
struct ProfileCpuScope {
    int label; LARGE_INTEGER started{};
    explicit ProfileCpuScope(int id);
    ~ProfileCpuScope();
    ProfileCpuScope(const ProfileCpuScope&) = delete;
    ProfileCpuScope& operator=(const ProfileCpuScope&) = delete;
};
#define PROFILE_LABEL(name) ([]{ static const int id = ProfilerLabel(name); return id; }())

extern "C" __declspec(dllexport) void __cdecl RiskOfRainUpscaling_ProfilerSetEnabled(int);
// Registers a managed label; returns its id for the profiler event.
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_ProfilerRegister(const char*);
// Describes the current configuration; a change starts a new report window.
extern "C" __declspec(dllexport) void __cdecl RiskOfRainUpscaling_ProfilerSetContext(const char*);
// Render event: ID = label id << 2 | kind (0 begin, 1 end, 2 instant).
extern "C" __declspec(dllexport) void* __cdecl RiskOfRainUpscaling_ProfilerGetEvent();
