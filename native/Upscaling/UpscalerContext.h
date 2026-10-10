#pragma once
#include "SceneInputs/SceneInputs.h"
#include "Core/CameraFrame.h"

struct UpscalerDescription {
    unsigned size, version;
    int width, height;
    void* sceneInputs; void* color; void* depth; void* motion; void* output;
    // Version 2: width/height above are the render size; output is display-sized.
    // quality: 0 DLAA, 1 Quality, 2 Balanced, 3 Performance, 4 Ultra Performance.
    // placement: 0 end of effects (post-tonemap color); 1 before post-processing
    // (HDR scene color after transparents; RCAS via the sharpen event). DLAA writes
    // the scene in place; lower-resolution DLSS writes the display-sized output,
    // which the post stack then reads at display size.
    int outputWidth, outputHeight, quality, placement;
};
extern "C" __declspec(dllexport) void* __cdecl RiskOfRainUpscaling_UpscalerCreate(const UpscalerDescription*);
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_UpscalerSubmitFrame(void*, const CameraFrame*);
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_SetSharpness(float);
extern "C" __declspec(dllexport) void* __cdecl RiskOfRainUpscaling_UpscalerGetEvaluateEvent();
// Before-post-processing placement: RCAS at the effect tail. DLAA sharpens color
// into output; lower-resolution DLSS sharpens output in place.
extern "C" __declspec(dllexport) void* __cdecl RiskOfRainUpscaling_UpscalerGetSharpenEvent();
// Before-post-processing upscaling: frames DLSS does not evaluate still give the
// post stack a display-sized image (bilinear, NaN/Inf removed). Event ID = Unity frame.
extern "C" __declspec(dllexport) void* __cdecl RiskOfRainUpscaling_UpscalerGetFallbackEvent();
// Before-post-processing upscaling: registers Unity's render-sized outline mask
// (ARGB32, shader resource) and its display-sized target (ARGB32, render target).
// The outline event then upscales one into the other with edges one display pixel
// wide (OutlineMask.hlsl). Event ID = Unity frame.
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_UpscalerSetOutlineMasks(void*, void*, void*);
extern "C" __declspec(dllexport) void* __cdecl RiskOfRainUpscaling_UpscalerGetOutlineUpscaleEvent();
// Before post-processing (DLAA and upscaling), before the ready event: temporal
// outlines. mask is Unity's render-sized ARGB32 highlight mask and sobel its
// R8 Sobel edges, both drawn with the jittered projection before our evaluation;
// composite is a display-sized ARGBHalf texture. The outline weight rides in the
// DLSS input alpha (alpha upscaling); the composite event (ID = Unity frame)
// blends the reconstructed outline into composite in place.
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_UpscalerSetTemporalOutlines(void*, void*, void*, void*);
// 1 when the DLSS feature accepted alpha upscaling for temporal outlines.
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_UpscalerTemporalOutlinesActive(void*);
extern "C" __declspec(dllexport) void* __cdecl RiskOfRainUpscaling_UpscalerGetOutlineCompositeEvent();
// Before-post-processing upscaling: full-resolution outlines. mask is the
// display-sized ARGB32 highlight mask Unity draws; objectDepth the display-sized
// depth texture of the highlighted objects (far where none); occlusion a
// display-sized RFloat and sobel a display-sized RHalf render target;
// reversedDepth is 1 for reversed-Z. The occlusion event (ID = Unity frame)
// writes, per display pixel, the farthest raw depth among the nearby render
// texels, which the game's fill pass tests against. The Sobel event (ID = Unity
// frame), after the mask and object depth are drawn, writes the game's Sobel
// edges anti-aliased at the display size, encoded for its composite pass.
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_UpscalerSetFullResolutionOutlines(void*, void*, void*, void*, void*, int);
// 1 while full-resolution outlines are registered and have not failed.
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_UpscalerFullResolutionOutlinesActive(void*);
extern "C" __declspec(dllexport) void* __cdecl RiskOfRainUpscaling_UpscalerGetOutlineOcclusionEvent();
extern "C" __declspec(dllexport) void* __cdecl RiskOfRainUpscaling_UpscalerGetOutlineSobelEvent();
// Super-resolution backend for contexts created afterwards: 0 Auto, 1 DLSS, 2 FSR.
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_SetUpscalerBackend(int);
// The running backend of a context: 0 none yet, 1 DLSS, 2 FSR.
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_UpscalerKind(void*);
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_UpscalerState(void*);
extern "C" __declspec(dllexport) void __cdecl RiskOfRainUpscaling_UpscalerDestroy(void*);
// Render-thread validation of the shared scene inputs.
__declspec(dllexport) bool SceneInputsReady(void*, int, ID3D11Device*);
// The scene color target bound after transparents for this Unity frame, or null.
__declspec(dllexport) ID3D11Texture2D* SceneInputsSceneColor(void*, int, ID3D11Device*);
