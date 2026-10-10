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
    // Diagnostic capture (Capture Diagnostics): exports one frame's images and capture.json.
    internal sealed partial class RenderPipeline
    {
        private IEnumerator Capture(PipelineResources resources)
        {
            var capture = CaptureCore(resources);
            try
            {
                while (true)
                {
                    bool more;
                    try { more = capture.MoveNext(); }
                    catch (Exception exception) { LogError("CAPTURE_FAILED " + exception); yield break; }
                    if (!more) yield break;
                    yield return capture.Current;
                }
            }
            finally { (capture as IDisposable)?.Dispose(); }
        }
        private IEnumerator CaptureCore(PipelineResources resources)
        {
            try
            {
                resources.PrepareCapture(Time.frameCount);
                Log("CAPTURE_REQUESTED frame=" + resources.RequestFrame);
                float deadline = Time.realtimeSinceStartup + 5f;
                while (true)
                {
                    yield return new WaitForEndOfFrame();
                    if (active != resources || disposed || resources.Diagnostics == IntPtr.Zero) yield break;
                    var state = NativeApi.CaptureState(resources.Diagnostics);
                    if (state < 0) throw new InvalidOperationException("Native capture failed; see native.log");
                    if (state == 3) break;
                    if (Time.realtimeSinceStartup > deadline) throw new TimeoutException("Capture/GPU acknowledgement timed out");
                    var poll = new CommandBuffer { name = "RiskOfRainUpscaling.Diagnostics.CapturePoll" };
                    try { poll.IssuePluginEventAndData(NativeApi.DiagnosticsGetEvent(), 2, resources.Diagnostics); Graphics.ExecuteCommandBuffer(poll); }
                    finally { poll.Release(); }
                    yield return null;
                }
                if (resources.CapturedScene == null) throw new InvalidOperationException("Requested scene-frame metadata missing");
                string captureFolder = Path.Combine(folder, "captures", DateTime.UtcNow.ToString("yyyyMMdd-HHmmss-fff"));
                Directory.CreateDirectory(captureFolder);
                SavePng(resources.Frozen[6], Path.Combine(captureFolder,"final.png"));
                SavePng(resources.Frozen[0], Path.Combine(captureFolder,"scene-color.png"));
                SavePng(resources.Frozen[1], Path.Combine(captureFolder,"scene-before-hud.png"));
                SavePng(resources.Frozen[4], Path.Combine(captureFolder,"depth-preview.png"));
                SavePng(resources.Frozen[5], Path.Combine(captureFolder,"motion-preview.png"));
                var metadata = Metadata(resources, ReadFloat(resources.Frozen[2]), ReadFloat(resources.Frozen[3]));
                File.WriteAllText(Path.Combine(captureFolder,"capture.json"), JsonUtility.ToJson(metadata,true));
                Log("CAPTURE_COMPLETE unityFrame=" + metadata.frame + " exportUnityFrame=" + Time.frameCount + " folder=" + captureFolder +
                    " motionMaxUV=" + metadata.motionMax + " motionNonfinite=" + metadata.motionNonfinite + " depthNonfinite=" + metadata.depthNonfinite);
            }
            finally
            {
                if (resources.Diagnostics != IntPtr.Zero) NativeApi.CaptureCancel(resources.Diagnostics);
                resources.ReleaseCapture();
                capturing = false;
            }
        }
        private static Texture2D Read(RenderTexture texture, bool floating)
        {
            var previous = RenderTexture.active;
            var readback = new Texture2D(texture.width,texture.height,floating ? TextureFormat.RGBAFloat : TextureFormat.RGBA32,false,floating);
            try { RenderTexture.active = texture; readback.ReadPixels(new Rect(0,0,texture.width,texture.height),0,0,false); readback.Apply(false,false); return readback; }
            catch { UnityEngine.Object.Destroy(readback); throw; }
            finally { RenderTexture.active = previous; }
        }
        private static void SavePng(RenderTexture texture, string path)
        {
            var read = Read(texture,false);
            try { File.WriteAllBytes(path,read.EncodeToPNG()); }
            finally { UnityEngine.Object.Destroy(read); }
        }
        // Raw depth is RFloat and motion RGHalf; expand them to RGBA float before readback.
        private static Color[] ReadFloat(RenderTexture texture)
        {
            var expanded = texture.format == RenderTextureFormat.ARGBFloat ? null :
                RenderTexture.GetTemporary(texture.width, texture.height, 0, RenderTextureFormat.ARGBFloat, RenderTextureReadWrite.Linear);
            try
            {
                if (expanded) Graphics.Blit(texture, expanded);
                var read = Read(expanded ? expanded : texture, true);
                try { return read.GetPixels(); }
                finally { UnityEngine.Object.Destroy(read); }
            }
            finally { if (expanded) RenderTexture.ReleaseTemporary(expanded); }
        }
        [Serializable] internal sealed class CaptureMetadata
        {
            public string version=Plugin.Version, utc, scene, colorSpace, presentationMode, reconstructionMode="Off", dlssQuality="none", upscaler="none", upscalerColorInput="none",
                sceneColorCapturePoint="scene AfterEverything: end of effect chain before UI", sceneBeforeHudCapturePoint="scene AfterEverything",
                finalCapturePoint="HUD camera AfterEverything: before Screen Space - Overlay canvases and IMGUI; rows flipped to Unity orientation",
                depthEncoding="raw hardware depth from same-frame native DSV copy", motionEncoding="Unity signed screen UV offsets",
                scenePngEncoding, finalPngEncoding="srgb";
            public int frame,inputWidth,inputHeight,outputWidth,outputHeight,actualCameraWidth,actualCameraHeight,scaledCameraWidth,scaledCameraHeight,hudWidth,hudHeight;
            public int upscalerStateAtScene,depthNonfinite,motionNonfinite,motionNonzeroPixels;
            public bool lowerResolution,reversedZ;
            public float near,far,depthMin,depthMax,motionMax,motionPreviewGain,depthPreviewRange;
            public Matrix4x4 projection,nonJitteredProjection,worldToCamera;
        }
        private static CaptureMetadata SceneMetadata(PipelineResources resources)
        {
            var result = new CaptureMetadata { utc=DateTime.UtcNow.ToString("O"), scene=resources.Scene.gameObject.scene.name,frame=Time.frameCount,
                colorSpace=QualitySettings.activeColorSpace.ToString(),inputWidth=resources.InputWidth,inputHeight=resources.InputHeight,outputWidth=resources.Width,outputHeight=resources.Height,
                actualCameraWidth=resources.Scene.pixelWidth,actualCameraHeight=resources.Scene.pixelHeight,hudWidth=resources.Hud.pixelWidth,hudHeight=resources.Hud.pixelHeight,
                scaledCameraWidth=resources.Scene.scaledPixelWidth,scaledCameraHeight=resources.Scene.scaledPixelHeight,
                lowerResolution=resources.Low,reversedZ=SystemInfo.usesReversedZBuffer,near=resources.Near,far=resources.Far,depthMin=float.PositiveInfinity,depthMax=float.NegativeInfinity,
                motionPreviewGain=MotionPreviewGain,depthPreviewRange=DepthPreviewRange,
                projection=resources.Scene.projectionMatrix,nonJitteredProjection=resources.Scene.nonJitteredProjectionMatrix,worldToCamera=resources.Scene.worldToCameraMatrix };
            result.scenePngEncoding = QualitySettings.activeColorSpace == ColorSpace.Linear ? "linear" : "srgb";
            if (resources.Low || resources.Session!=null) result.sceneColorCapturePoint = "explicit source of appended final scene OnRenderImage";
            if (resources.Session!=null) {
                result.reconstructionMode=resources.Session.Quality==DlssQuality.DLAA?"DLAA":"DLSS";result.dlssQuality=resources.Session.Quality.ToString();
                result.upscaler=NativeApi.UpscalerName(resources.Session.Context);result.upscalerStateAtScene=resources.Session.State;
                result.sceneBeforeHudCapturePoint="upscaler output at end of effects before HUD";
                result.upscalerColorInput=resources.Session.BeforePostProcessing?(resources.Session.Upscaled?"linear HDR scene after transparents (NaN/Inf removed); upscaled before display-sized post-processing":"linear HDR scene after transparents (NaN/Inf removed); evaluated in place"):"sRGB-encoded, decoded after evaluation";
            }
            return result;
        }
        private static CaptureMetadata Metadata(PipelineResources resources, Color[] depth, Color[] motion)
        {
            var result = resources.CapturedScene;
            result.presentationMode=NativeApi.PresentationLabel;
            if(result.presentationMode=="D3D12 bridge")result.finalCapturePoint+="; D3D11 producer image, not the physical D3D12 display buffer";
            foreach (var value in depth) { if (!Finite(value.r)) ++result.depthNonfinite; else { result.depthMin=Math.Min(result.depthMin,value.r); result.depthMax=Math.Max(result.depthMax,value.r); } }
            foreach (var value in motion) { if (!Finite(value.r)||!Finite(value.g)) ++result.motionNonfinite; else { float length=new Vector2(value.r,value.g).magnitude; result.motionMax=Math.Max(result.motionMax,length); if (length>1e-6) ++result.motionNonzeroPixels; } }
            if(!Finite(result.depthMin)) result.depthMin=0;if(!Finite(result.depthMax))result.depthMax=0;
            return result;
        }
        private static bool Finite(float value) { return !float.IsNaN(value)&&!float.IsInfinity(value); }
    }
}
