using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using BepInEx.Configuration;
using RoR2;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.PostProcessing;
using Path = System.IO.Path;

namespace RiskOfRainUpscaling
{
    // Debug overlay (Toggle Debug Overlay): presentation/upscaler/FG status and preview panels.
    internal sealed partial class RenderPipeline
    {
        internal void DrawOverlay()
        {
            if (disposed || !overlay.Value || active == null || Event.current.type != EventType.Repaint) return;
            var resources = active;
            float width = Math.Min(Screen.width / 3f, 400f), height = width * resources.Height / resources.Width;
            GUI.Box(new Rect(0,0,width*3,height*2+66), "");
            string fgKey = settings.HotkeysEnabled.Value ? settings.ToggleFrameGeneration.Value.ToString() : "hotkeys disabled";
            string captureKey = settings.HotkeysEnabled.Value ? settings.CaptureEvidence.Value.ToString() : "hotkeys disabled";
            GUI.Label(new Rect(8,2,width*3-16,24), NativeApi.PresentationLabel + (active?.Session!=null ? " | SR " + NativeApi.UpscalerName(active.Session.Context) : "") + " | FG " + NativeApi.FrameGenerationLabel + " (" + fgKey + ") | " + fgTiming?.LimiterStatus + " | SDK presents " + NativeApi.FrameGenerationPresentedFrames());
            if (!resources.DiagnosticsReady) { GUI.Label(new Rect(8,28,width*3-16,24), "Diagnostics unavailable for this camera; see native.log."); return; }
            Panel(resources.Color, "Scene color " + resources.InputWidth + "×" + resources.InputHeight, 0, 28, width, height);
            Panel(resources.DepthView, "Depth (log distance)", width, 28, width, height);
            Panel(resources.MotionView, "Motion (UV ×" + MotionPreviewGain + ")", width*2, 28, width, height);
            Panel(resources.HudFree, "Before HUD", 0, height+46, width, height);
            Panel(resources.Final, "After HUD; capture: " + captureKey, width, height+46, width, height);
        }
        private static void Panel(Texture texture, string label, float x, float y, float width, float height)
        {
            GUI.DrawTexture(new Rect(x+4,y,width-8,height-4), texture, ScaleMode.ScaleToFit, false);
            GUI.Label(new Rect(x+6,y+height-2,width-12,20), label);
        }
    }
}
