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
    // Optional diagnostics resources: created on first use, recorded only while wanted.
    internal sealed partial class PipelineResources
    {
        internal void EnsureDiagnostics(Action<string> log)
        {
            if(Diagnostics!=IntPtr.Zero || diagnosticsUnavailable) return;
            try {
                DepthView=DepthView?DepthView:Texture("DepthPreview",InputWidth,InputHeight,RenderTextureFormat.ARGB32);
                MotionView=MotionView?MotionView:Texture("MotionPreview",InputWidth,InputHeight,RenderTextureFormat.ARGB32);
                Final=Final?Final:Texture("Final",Width,Height,RenderTextureFormat.ARGB32);
                var description=new NativeApi.DiagnosticsDescription{Size=(uint)Marshal.SizeOf<NativeApi.DiagnosticsDescription>(),Version=1,InputWidth=InputWidth,InputHeight=InputHeight,
                    OutputWidth=Width,OutputHeight=Height,Near=Near,Far=Far,DepthRange=RenderPipeline.DepthPreviewRange,MotionGain=RenderPipeline.MotionPreviewGain,ReversedDepth=SystemInfo.usesReversedZBuffer?1:0,
                    Depth=Depth.GetNativeTexturePtr(),Motion=Motion.GetNativeTexturePtr(),DepthPreview=DepthView.GetNativeTexturePtr(),MotionPreview=MotionView.GetNativeTexturePtr(),
                    FinalColor=Final.GetNativeTexturePtr(),SceneColor=Color.GetNativeTexturePtr(),SceneBeforeHud=HudFree.GetNativeTexturePtr()};
                Diagnostics=NativeApi.DiagnosticsCreate(ref description);
                if(Diagnostics==IntPtr.Zero) throw new InvalidOperationException("native diagnostics context unavailable");
            } catch(Exception exception) {
                diagnosticsUnavailable=true;
                log("DIAGNOSTICS_UNAVAILABLE "+exception.Message+"; overlay previews and capture are off for this camera.");
                return;
            }
            // Added after the core buffers: previews read raw depth/motion, which upscaling never modifies.
            DiagnosticPreviews=new CommandBuffer{name="RiskOfRainUpscaling.Diagnostics.Previews"};
            Scene.AddCommandBuffer(CameraEvent.AfterForwardAlpha,DiagnosticPreviews);
            DiagnosticFinal=new CommandBuffer{name="RiskOfRainUpscaling.Diagnostics.FinalCopy"};
            Hud.AddCommandBuffer(CameraEvent.AfterEverything,DiagnosticFinal);
        }
        // Re-recorded every frame: no diagnostics GPU work unless the overlay is shown or a capture runs.
        internal void RecordDiagnostics(bool wanted)
        {
            if(DiagnosticPreviews==null) return;
            DiagnosticPreviews.Clear();DiagnosticFinal.Clear();
            if(!wanted || !DiagnosticsReady) return;
            DiagnosticPreviews.IssuePluginEventAndData(NativeApi.DiagnosticsGetEvent(),1,Diagnostics);
            DiagnosticFinal.IssuePluginEventAndData(NativeApi.DiagnosticsGetFinalCopyEvent(),Time.frameCount,Diagnostics);
        }
        internal void PrepareCapture(int frame)
        {
            ReleaseCapture();
            var sources = new[]{Color,HudFree,Depth,Motion,DepthView,MotionView,Final};
            Frozen = sources.Select(source => Texture("Frozen."+source.name,source.width,source.height,source.format,0,false)).ToArray();
            RequestFrame=frame; CapturedScene=null;
            var request = new NativeApi.CaptureDescription { Size=(uint)Marshal.SizeOf<NativeApi.CaptureDescription>(),Version=1,UnityFrame=frame,
                SceneColor=Frozen[0].GetNativeTexturePtr(),SceneBeforeHud=Frozen[1].GetNativeTexturePtr(),Depth=Frozen[2].GetNativeTexturePtr(),Motion=Frozen[3].GetNativeTexturePtr(),
                DepthPreview=Frozen[4].GetNativeTexturePtr(),MotionPreview=Frozen[5].GetNativeTexturePtr(),FinalColor=Frozen[6].GetNativeTexturePtr() };
            if(NativeApi.CaptureArm(Diagnostics,ref request)!=0) throw new InvalidOperationException("Native capture arm rejected");
        }
        internal void ReleaseCapture()
        {
            if(Frozen==null) return;
            foreach(var texture in Frozen) if(texture) { texture.Release(); UnityEngine.Object.Destroy(texture); }
            Frozen=null;
        }
    }
}
