#pragma once
#include "Core/Native.h"

// Optional diagnostics (debug overlay previews, final-image copy and the F8
// capture). Its own context: a failure here never affects scene inputs or
// upscaling. Managed code records its events only while the overlay is shown
// or a capture is armed.
struct DiagnosticsDescription
{
    unsigned size, version;
    int inputWidth, inputHeight, outputWidth, outputHeight;
    float nearPlane, farPlane, depthRange, motionGain;
    int reversedDepth, reserved;
    void* depth;          // Raw depth (render size, shader resource).
    void* motion;         // Raw motion (render size, shader resource).
    void* depthPreview;   // Render size, render target.
    void* motionPreview;  // Render size, render target.
    void* finalColor;     // Display size 8-bit render target: vertically flipped HUD camera image.
    void* sceneColor;     // Capture source: render-sized scene color.
    void* sceneBeforeHud; // Capture source: display-sized scene before the HUD.
};
// Capture destinations match their sources' size and format and are never the sources.
struct CaptureDescription
{
    unsigned size, version;
    int unityFrame, reserved;
    void* sceneColor; void* sceneBeforeHud; void* depth; void* motion;
    void* depthPreview; void* motionPreview; void* finalColor;
};
enum DiagnosticsStage { DiagnosticsPreviews = 1, DiagnosticsCapturePoll = 2, DiagnosticsRetire = -1 };
extern "C" __declspec(dllexport) void* __cdecl RiskOfRainUpscaling_DiagnosticsCreate(const DiagnosticsDescription*);
// Event ID: a DiagnosticsStage.
extern "C" __declspec(dllexport) void* __cdecl RiskOfRainUpscaling_DiagnosticsGetEvent();
// Event ID = Unity frame. Copies the bound HUD camera target into finalColor and
// freezes an armed capture when the frame matches.
extern "C" __declspec(dllexport) void* __cdecl RiskOfRainUpscaling_DiagnosticsGetFinalCopyEvent();
// 0 ready, 1 used, -2 failed, -1 retired.
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_DiagnosticsState(void*);
extern "C" __declspec(dllexport) void __cdecl RiskOfRainUpscaling_DiagnosticsDestroy(void*);
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_CaptureArm(void*, const CaptureDescription*);
// 0 idle, 1 armed, 2 submitted, 3 GPU complete, -1 failed.
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_CaptureState(void*);
// Returns to idle and releases the capture destinations.
extern "C" __declspec(dllexport) void __cdecl RiskOfRainUpscaling_CaptureCancel(void*);
