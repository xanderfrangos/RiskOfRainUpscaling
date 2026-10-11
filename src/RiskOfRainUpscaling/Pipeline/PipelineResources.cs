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
    // The scene/HUD/skybox cameras and owned render targets of one attachment, the
    // native scene-input context, and world routing into the HUD camera's target.
    internal sealed partial class PipelineResources
    {
        // A callback still in Unity's queue owns these contexts and textures; keep them until process exit.
        internal static readonly List<PipelineResources> RetainedOnExit = new List<PipelineResources>();
        internal UpscalerSession Session;
        internal Camera Scene,Hud,Sky;
        internal SceneTail Tail;
        internal RenderTexture World,Color,Depth,Motion,HudFree;
        // Diagnostics: created on first use (overlay or capture); Frozen only while a capture runs.
        internal RenderTexture DepthView,MotionView,Final;
        internal RenderTexture[] Frozen;
        internal int RequestFrame;
        internal RenderPipeline.CaptureMetadata CapturedScene;
        internal CommandBuffer Geometry,Before,After,Presentation,FgBefore,FgAfter,FgFinished,UpscalePre,DiagnosticPreviews,DiagnosticFinal;
        internal IntPtr Context,Diagnostics;
        internal bool DiagnosticsFailureReported,diagnosticsUnavailable;
        internal bool DiagnosticsReady => Diagnostics!=IntPtr.Zero && NativeApi.DiagnosticsState(Diagnostics)>=0;
        internal int Width,Height,InputWidth,InputHeight;
        internal bool Low,SkyActive;
        internal float Near,Far,SceneOrder,HudOrder;
        internal readonly HashSet<string> OrderStates = new HashSet<string>();
        private readonly List<RenderTexture> textures = new List<RenderTexture>();
        private CameraClearFlags sceneClear,skyClear;
        private Color sceneBackground,skyBackground;
        private bool changedSceneClear,changedSkyClear;
        private readonly Dictionary<CameraResolutionScaler,bool> scalers = new Dictionary<CameraResolutionScaler,bool>();
        internal PipelineResources(CameraRigController rig,int width,int height,int inputWidth,int inputHeight,bool low)
        {
            Scene=rig.sceneCam;Hud=rig.uiCam;Sky=rig.skyboxCam;Width=width;Height=height;InputWidth=inputWidth;InputHeight=inputHeight;Low=low;
            Near=Scene.nearClipPlane;Far=Scene.farClipPlane;
            SkyActive=Sky && Sky.isActiveAndEnabled;SceneOrder=Scene.depth;HudOrder=Hud.depth;
            try
            {
                Color=Texture("SceneInput",inputWidth,inputHeight,RenderTextureFormat.ARGBHalf);
                // Single-channel depth and Unity's own two-channel half motion format:
                // the upscaler copies or reads them directly, without 128-bit intermediates.
                Depth=Texture("RawDepth",inputWidth,inputHeight,RenderTextureFormat.RFloat);
                Motion=Texture("RawMotion",inputWidth,inputHeight,RenderTextureFormat.RGHalf);
                HudFree=Texture("HudFree",width,height,RenderTextureFormat.ARGBHalf);
                if(low) World=Texture("LowResolutionWorld",inputWidth,inputHeight,RenderTextureFormat.ARGBHalf,24);
                var description=new NativeApi.SceneInputsDescription{Size=(uint)Marshal.SizeOf<NativeApi.SceneInputsDescription>(),Version=4,Width=inputWidth,Height=inputHeight,
                    Depth=Depth.GetNativeTexturePtr()};
                Context=NativeApi.SceneInputsCreate(ref description);
                if(Context==IntPtr.Zero) throw new InvalidOperationException("Native scene inputs context unavailable");
            }
            catch {
                // No render callbacks exist yet, so queue retirement and retain owned textures until process exit.
                if(Context!=IntPtr.Zero) {
                    var retire=new CommandBuffer();
                    try {retire.IssuePluginEventAndData(NativeApi.SceneInputsGetEvent(),199,Context);Graphics.ExecuteCommandBuffer(retire);}
                    finally {retire.Release();}
                    RetainedOnExit.Add(this);
                } else Release();
                throw;
            }
        }
        private RenderTexture Texture(string name,int width,int height,RenderTextureFormat format,int depth=0,bool owned=true)
        {
            var texture=new RenderTexture(width,height,depth,format,RenderTextureReadWrite.Linear){name="RiskOfRainUpscaling.Pipeline."+name,filterMode=FilterMode.Bilinear,antiAliasing=1,useMipMap=false,autoGenerateMips=false};
            if(owned) textures.Add(texture);
            if(!texture.Create()) { if(!owned) UnityEngine.Object.Destroy(texture); throw new InvalidOperationException("Texture creation failed: "+name); }
            var previous=RenderTexture.active;
            try {RenderTexture.active=texture;GL.Clear(true,true,UnityEngine.Color.clear);}
            finally {RenderTexture.active=previous;}
            return texture;
        }
        internal void Route()
        {
            foreach (var camera in new[]{Scene,Sky}.Where(c=>c && c.isActiveAndEnabled)) foreach (var scaler in camera.GetComponents<CameraResolutionScaler>())
            { scalers[scaler]=scaler.enabled;scaler.enabled=false; }
            Scene.targetTexture=World;
            DisplaySpace.Route(Scene,InputWidth,InputHeight,Width,Height);
            Tail=Scene.gameObject.AddComponent<SceneTail>();
            if(Sky && Sky.isActiveAndEnabled)
            {
                Sky.targetTexture=World; skyClear=Sky.clearFlags;skyBackground=Sky.backgroundColor;
                if(Sky.clearFlags==CameraClearFlags.Depth||Sky.clearFlags==CameraClearFlags.Nothing){Sky.clearFlags=CameraClearFlags.SolidColor;Sky.backgroundColor=UnityEngine.Color.black;changedSkyClear=true;}
            }
            else if(Scene.clearFlags==CameraClearFlags.Depth||Scene.clearFlags==CameraClearFlags.Nothing)
            {
                sceneClear=Scene.clearFlags;sceneBackground=Scene.backgroundColor;Scene.clearFlags=CameraClearFlags.SolidColor;Scene.backgroundColor=UnityEngine.Color.black;changedSceneClear=true;
            }
            // The HUD camera's DamageIndicator image effect makes Unity draw the
            // HUD into an intermediate, then copy that over the display. While
            // the scene camera targets the display, the scene writes the world
            // into that intermediate; once it targets World, nothing does, and
            // the HUD lands on a stale frame. Write the
            // upscaled world into whatever the HUD camera targets, before the HUD.
            Presentation=new CommandBuffer{name="RiskOfRainUpscaling.Routing.WorldIntoHudTarget"};
            Profiling.Begin(Presentation,"route.blit upscaled world -> HUD target");
            Presentation.Blit(HudFree,BuiltinRenderTextureType.CameraTarget);
            Profiling.End(Presentation,"route.blit upscaled world -> HUD target");
            Presentation.SetRenderTarget(BuiltinRenderTextureType.CameraTarget);
            Hud.AddCommandBuffer(CameraEvent.BeforeForwardOpaque,Presentation);
        }
        internal bool TailIsLast()
        {
            return Tail && Scene.GetComponents<MonoBehaviour>().Where(component => component && component.isActiveAndEnabled &&
                component.GetType().GetMethod("OnRenderImage",System.Reflection.BindingFlags.Instance|System.Reflection.BindingFlags.Public|System.Reflection.BindingFlags.NonPublic)!=null).LastOrDefault()==Tail;
        }
        internal void Restore()
        {
            Session?.Restore();
            foreach(var pair in scalers) if(pair.Key && !pair.Key.enabled)pair.Key.enabled=pair.Value;
            scalers.Clear();
            if(Scene){if(Geometry!=null)Scene.RemoveCommandBuffer(CameraEvent.AfterGBuffer,Geometry);if(Before!=null)Scene.RemoveCommandBuffer(CameraEvent.AfterForwardAlpha,Before);if(UpscalePre!=null)Scene.RemoveCommandBuffer(CameraEvent.AfterForwardAlpha,UpscalePre);if(After!=null)Scene.RemoveCommandBuffer(CameraEvent.AfterEverything,After);
                DisplaySpace.Clear(Scene);
                if(World && Scene.targetTexture==World)Scene.targetTexture=null;if(changedSceneClear && Scene.clearFlags==CameraClearFlags.SolidColor){Scene.clearFlags=sceneClear;Scene.backgroundColor=sceneBackground;}}
            if(Sky){if(World && Sky.targetTexture==World)Sky.targetTexture=null;if(changedSkyClear && Sky.clearFlags==CameraClearFlags.SolidColor){Sky.clearFlags=skyClear;Sky.backgroundColor=skyBackground;}}
            if(Scene && DiagnosticPreviews!=null)Scene.RemoveCommandBuffer(CameraEvent.AfterForwardAlpha,DiagnosticPreviews);
            if(Hud && DiagnosticFinal!=null)Hud.RemoveCommandBuffer(CameraEvent.AfterEverything,DiagnosticFinal);
            if(Hud && Presentation!=null)Hud.RemoveCommandBuffer(CameraEvent.BeforeForwardOpaque,Presentation);
            if(Hud && FgBefore!=null)Hud.RemoveCommandBuffer(CameraEvent.BeforeForwardOpaque,FgBefore);
            if(Hud && FgAfter!=null)Hud.RemoveCommandBuffer(CameraEvent.BeforeImageEffects,FgAfter);
            if(Hud && FgFinished!=null)Hud.RemoveCommandBuffer(CameraEvent.AfterEverything,FgFinished);
            if(Tail){Tail.Process=null;Tail.enabled=false;UnityEngine.Object.Destroy(Tail);Tail=null;}
            Geometry?.Release();Before?.Release();After?.Release();Presentation?.Release();Geometry=Before=After=Presentation=null;
            DiagnosticPreviews?.Release();DiagnosticFinal?.Release();DiagnosticPreviews=DiagnosticFinal=null;
            FgBefore?.Release();FgBefore=null;
            FgAfter?.Release();FgAfter=null;
            FgFinished?.Release();FgFinished=null;
            UpscalePre?.Release();UpscalePre=null;
        }
        internal void Release()
        {
            Session?.Release();
            ReleaseCapture();
            foreach(var texture in textures){if(!texture)continue;texture.Release();UnityEngine.Object.Destroy(texture);}textures.Clear();
        }
    }
}
