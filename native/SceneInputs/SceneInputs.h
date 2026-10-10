#pragma once
#include "Core/Native.h"

// Scene inputs: same-frame raw depth (copied from the scene DSV) and motion
// validation for the upscaler, plus the scene color bound after transparents.
struct SceneInputsDescription
{
    unsigned size, version;
    int width, height;
    void* depth; // Owned render-sized raw depth target (RFloat; ARGBFloat also accepted).
};
extern "C" __declspec(dllexport) void* __cdecl RiskOfRainUpscaling_SceneInputsCreate(const SceneInputsDescription*);
extern "C" __declspec(dllexport) void* __cdecl RiskOfRainUpscaling_SceneInputsGetEvent();
extern "C" __declspec(dllexport) void* __cdecl RiskOfRainUpscaling_SceneInputsGetFrameEvent();
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_SceneInputsState(void*);
extern "C" __declspec(dllexport) void __cdecl RiskOfRainUpscaling_SceneInputsDestroy(void*);
// Test hook: source dimensions and stage mask (2 depth, 4 motion, 8 G-buffer) of the latest frame.
struct SceneInputsMetrics { int frame, depthWidth, depthHeight, motionWidth, motionHeight, stageMask;
    int gbufferWidth, gbufferHeight, geometryDepthWidth, geometryDepthHeight; };
__declspec(dllexport) int SceneInputsGetMetrics(void*, SceneInputsMetrics*);
