using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using BepInEx.Configuration;
using HarmonyLib;
using RoR2;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.PostProcessing;

namespace RiskOfRainUpscaling
{
    // One upscaler (DLAA/DLSS/FSR) attached to a scene camera: jitter, history and the
    // native context. Partial files add scene fixups, display-sized post-processing and outlines.
    internal sealed partial class UpscalerSession
    {
        private static readonly Dictionary<PostProcessLayer,UpscalerSession> owners=new Dictionary<PostProcessLayer,UpscalerSession>();
        private static bool patched;
        internal Action<Exception> Failure;
        internal IntPtr Context;
        internal readonly RenderTexture Output;
        private readonly Camera camera;
        private readonly ModLog log;
        private readonly Dictionary<CameraResolutionScaler,bool> scalers = new Dictionary<CameraResolutionScaler,bool>();
        private readonly PostProcessLayer layer;
        private PostProcessLayer.Antialiasing oldAA;
        private bool changedAA, jittered, customProjection, oldTransparentJitter;
        private Matrix4x4 original, applied, originalNonJittered, previousVP;
        private Vector3 previousPosition;
        private Quaternion previousRotation;
        private float previousFov, previousTime;
        private int previousFrame, preparedFrame=-1, jitterIndex;
        private int hookReadyFrame=-1;
        private int resetCount, resetRun, longestResetRun;
        private int fovHistoryFrames;
        private float maxRetainedFovDelta;

        internal readonly DlssQuality Quality;
        internal readonly UpscalerBackend Upscaler;
        internal readonly DlssPreset Preset;
        // Upscaling: Full Resolution leaves DLSS alpha upscaling off (UpscalerSession.Outlines).
        internal readonly OutlineMode Outlines;
        internal readonly bool BeforePostProcessing;
        // Lower-resolution DLSS before post-processing: Output receives the upscaled
        // HDR scene and the post stack is rebuilt at display size into PostOutput.
        internal readonly bool Upscaled;
        internal readonly RenderTexture PostOutput;
        // Before post-processing: the render-sized outline mask. Upscaling adds a
        // display-sized mask (its edge-sharpened copy, or drawn at full resolution);
        // temporal outlines add the jittered Sobel edges and a display-sized texture
        // the outline is composited into.
        private readonly RenderTexture outlineMask, outlineDisplayMask, outlineSobel, outlineComposite;
        private bool outlineSharpened, temporalRegistered, temporalTextures;
        private readonly int renderWidth, renderHeight, jitterPhases;
        private readonly float mipBias;

        internal UpscalerSession(Camera camera, IntPtr sceneInputs, RenderTexture color, RenderTexture depth, RenderTexture motion,
            int outputWidth, int outputHeight, DlssQuality quality, DlaaPlacement placement, float mipBiasOffset, ModLog log, UpscalerBackend upscaler=UpscalerBackend.Auto, DlssPreset preset=DlssPreset.Default, OutlineMode outlines=OutlineMode.FullResolution)
        {
            this.camera=camera; this.log=log; Quality=quality; Upscaler=upscaler; Preset=preset; Outlines=outlines; BeforePostProcessing=DlssPolicy.BeforePostProcessing(quality,placement);
            Upscaled=DlssPolicy.UpscalesBeforePostProcessing(quality,placement);
            renderWidth=color.width; renderHeight=color.height;
            if((quality==DlssQuality.DLAA)!=(outputWidth==renderWidth && outputHeight==renderHeight))
                throw new ArgumentException("Render/output sizes do not match the DLSS quality mode");
            jitterPhases=DlssPolicy.JitterPhases(renderWidth,outputWidth);
            mipBias=DlssPolicy.MipBias(renderWidth,outputWidth,mipBiasOffset);
            layer=camera.GetComponent<PostProcessLayer>();
            Output=new RenderTexture(outputWidth,outputHeight,0,RenderTextureFormat.ARGBHalf,RenderTextureReadWrite.Linear) {
                name="RiskOfRainUpscaling.Upscaling.Output", antiAliasing=1, useMipMap=false, autoGenerateMips=false, filterMode=FilterMode.Bilinear
            };
            if(Upscaled) PostOutput=new RenderTexture(outputWidth,outputHeight,0,RenderTextureFormat.ARGBHalf,RenderTextureReadWrite.Linear) {
                name="RiskOfRainUpscaling.Upscaling.DisplayPostProcessing", antiAliasing=1, useMipMap=false, autoGenerateMips=false, filterMode=FilterMode.Bilinear
            };
            try {
                if(!Output.Create()) throw new InvalidOperationException("DLSS output texture allocation failed");
                if(PostOutput && !PostOutput.Create()) throw new InvalidOperationException("Display-sized post-processing texture allocation failed");
                var d=new NativeApi.UpscalerDescription {Size=(uint)Marshal.SizeOf<NativeApi.UpscalerDescription>(),Version=2,
                    Width=renderWidth,Height=renderHeight,SceneInputs=sceneInputs,Color=color.GetNativeTexturePtr(),Depth=depth.GetNativeTexturePtr(),Motion=motion.GetNativeTexturePtr(),Output=Output.GetNativeTexturePtr(),
                    OutputWidth=outputWidth,OutputHeight=outputHeight,Quality=(int)quality,Placement=BeforePostProcessing?1:0};
                // The native context captures the backend preference at creation.
                if(NativeApi.SetUpscalerBackend((int)upscaler)!=0) throw new InvalidOperationException("Upscaler preference rejected");
                if(NativeApi.SetDlssPreset(DlssPolicy.NativePreset(preset))!=0) throw new InvalidOperationException("DLSS preset rejected");
                Context=NativeApi.UpscalerCreate(ref d);
                if(Context==IntPtr.Zero) throw new InvalidOperationException("Native DLAA setup failed; see native.log");
            } catch { Release(); throw; }
            if(BeforePostProcessing) {
                outlineMask=OutlineTexture("RiskOfRainUpscaling.Upscaling.OutlineMask",renderWidth,renderHeight,RenderTextureFormat.ARGB32);
                bool mask=outlineMask.Create();
                // Optional: registered before the ready event, which sets DLSS alpha upscaling.
                // Full-resolution outlines create their own display-sized textures when first drawn.
                if(TemporalOutlinesWanted) {
                    outlineSobel=OutlineTexture("RiskOfRainUpscaling.Upscaling.OutlineSobel",renderWidth,renderHeight,RenderTextureFormat.R8);
                    outlineComposite=OutlineTexture("RiskOfRainUpscaling.Upscaling.OutlineComposite",outputWidth,outputHeight,RenderTextureFormat.ARGBHalf);
                    // Registered in Initialize, once the game members needed to draw them are known.
                    temporalTextures=mask && SystemInfo.SupportsRenderTextureFormat(RenderTextureFormat.R8) && outlineSobel.Create() && outlineComposite.Create();
                }
                if(Upscaled) {
                    // Optional: without it the outline mask upscales bilinearly.
                    outlineDisplayMask=OutlineTexture("RiskOfRainUpscaling.Upscaling.OutlineMaskDisplay",outputWidth,outputHeight,RenderTextureFormat.ARGB32);
                    outlineSharpened=mask && outlineDisplayMask.Create() &&
                        NativeApi.UpscalerSetOutlineMasks(Context,outlineMask.GetNativeTexturePtr(),outlineDisplayMask.GetNativeTexturePtr())==0;
                    if(!outlineSharpened) log.Warning("OUTLINES sharpened mask upscale unavailable; bilinear outline mask");
                }
            }
        }
        private static RenderTexture OutlineTexture(string name,int width,int height,RenderTextureFormat format,FilterMode filter=FilterMode.Bilinear) =>
            new RenderTexture(width,height,0,format,RenderTextureReadWrite.Linear) {
                name=name, antiAliasing=1, useMipMap=false, autoGenerateMips=false, filterMode=filter
            };
        private bool TemporalOutlinesWanted => !(Upscaled && Outlines==OutlineMode.FullResolution);
        internal void Initialize()
        {
            if(layer && layer.isActiveAndEnabled) {
                if(!patched) {
                    var method=AccessTools.DeclaredMethod(typeof(PostProcessLayer),"OnPreCull");
                    if(method==null) throw new MissingMethodException("PostProcessLayer.OnPreCull");
                    new Harmony("local.riskofrainupscaling.upscaling.camera").Patch(method,
                        prefix:new HarmonyMethod(typeof(UpscalerSession),nameof(PreparePostProcess)),
                        postfix:new HarmonyMethod(typeof(UpscalerSession),nameof(AfterPostProcess)));
                    patched=true;
                }
                if(owners.ContainsKey(layer)) throw new InvalidOperationException("DLAA already owns this post-processing camera");
                if(BeforePostProcessing) PatchOutlines(log,Upscaled);
                if(Upscaled) PatchDisplayPostProcessing();
                owners.Add(layer,this);
                // The game's PPv2 caches its command buffers until volumes change.
                // Force a rebuild so the post stack picks up (or drops) our redirect.
                if(Upscaled) layer.OnGlobalVolumesDirty();
            }
            else if(Upscaled) throw new InvalidOperationException("Before-post-processing upscaling requires the scene camera's post-processing layer");
            foreach(var scaler in camera.GetComponents<CameraResolutionScaler>()) { scalers[scaler]=scaler.enabled; scaler.enabled=false; }
            // Before the ready event, which turns on DLSS alpha upscaling for registered
            // outlines. Without the game members nothing draws them, so alpha
            // upscaling and the outline passes would be wasted work.
            if(BeforePostProcessing && TemporalOutlinesWanted) {
                temporalRegistered=temporalTextures && recordMembers &&
                    NativeApi.UpscalerSetTemporalOutlines(Context,outlineMask.GetNativeTexturePtr(),outlineSobel.GetNativeTexturePtr(),outlineComposite.GetNativeTexturePtr())==0;
                if(!temporalRegistered) log.Warning("OUTLINES temporal outlines unavailable; outlines drawn after reconstruction without DLSS alpha upscaling");
            }
            var commands=new CommandBuffer {name="RiskOfRainUpscaling.Upscaling.Initialize"};
            try {commands.IssuePluginEventAndData(NativeApi.UpscalerGetEvaluateEvent(),-2,Context);Graphics.ExecuteCommandBuffer(commands);}
            finally {commands.Release();}
            ApplyMipBias();
            ApplyParticleMotion();
            log.Info("UPSCALER_ATTACHED quality="+Quality+" dlssPreset="+Preset+(Upscaled?" outlines="+Outlines:"")+" render="+renderWidth+"x"+renderHeight+" output="+Output.width+"x"+Output.height+
                " jitterPhases="+jitterPhases+" mipBias="+mipBias.ToString("F3",CultureInfo.InvariantCulture)+
                (Upscaled?"; HDR scene after transparents upscaled before post-processing; post stack, outlines and blur at display size; RCAS after post-processing":
                 BeforePostProcessing?"; HDR scene after transparents, before post-processing; RCAS after post-processing":"; final scene effect before HUD")+"; awaiting native ready/evaluation");
        }
        internal int State => Context==IntPtr.Zero?-1:NativeApi.UpscalerState(Context);
        internal bool Prepared => preparedFrame==Time.frameCount && State>0;
        internal bool UsesPostProcessHook => layer && layer.isActiveAndEnabled;
        internal bool ProjectionIntact => !Prepared || Approximately(camera.projectionMatrix,applied);
        private static void PreparePostProcess(PostProcessLayer __instance)
        {
            if(!owners.TryGetValue(__instance,out var owner))return;
            owner.hookReadyFrame=-1;
            try {if(owner.State>0){owner.DisableAA();owner.hookReadyFrame=Time.frameCount;}}
            catch(Exception e){var failure=owner.Failure;owner.Restore();failure?.Invoke(e);}
        }
        private static void AfterPostProcess(PostProcessLayer __instance)
        {
            if(!owners.TryGetValue(__instance,out var owner))return;
            if(owner.hookReadyFrame!=Time.frameCount)return;
            try {owner.Prepare();}catch(Exception e){var failure=owner.Failure;owner.Restore();failure?.Invoke(e);}
        }
        private void DisableAA()
        {
            if(!layer || !layer.isActiveAndEnabled)return;
            if(!changedAA) {oldAA=layer.antialiasingMode;changedAA=true;log.Info("POST_AA_DISABLED original="+oldAA+" temporary=None");}
            else if(layer.antialiasingMode!=PostProcessLayer.Antialiasing.None) throw new InvalidOperationException("Another component changed scene anti-aliasing while DLAA was active");
            layer.antialiasingMode=PostProcessLayer.Antialiasing.None;
        }

        internal void Prepare()
        {
            RestoreProjection(); preparedFrame=-1;
            if(State<1 || !camera || camera.orthographic) return;
            int frame=Time.frameCount;
            // A second render of the same scene camera is not another temporal frame.
            if(frame==previousFrame) return;
            DisableAA();
            original=camera.projectionMatrix; originalNonJittered=camera.nonJitteredProjectionMatrix;
            customProjection=!Approximately(original,ImplicitProjection());
            var gpu=GL.GetGPUProjectionMatrix(original,true);
            var currentVP=gpu*camera.worldToCameraMatrix;
            float now=Time.realtimeSinceStartup;
            float elapsed=previousFrame==0?0:now-previousTime;
            float distance=previousFrame==0?0:Vector3.Distance(camera.transform.position,previousPosition);
            float angle=previousFrame==0?0:Quaternion.Angle(camera.transform.rotation,previousRotation);
            float fovDelta=previousFrame==0?0:Math.Abs(camera.fieldOfView-previousFov);
            var reasons=DlaaHistoryPolicy.Reasons(previousFrame,frame,elapsed,distance,angle,fovDelta);
            bool reset=reasons!=DlaaResetReason.None;
            if(reset) {jitterIndex=0;previousVP=currentVP;}
            // Jitter is one render pixel wide; upscaling needs more phases (DLSS guide 3.7).
            int sample=(jitterIndex++%jitterPhases)+1;
            float x=Halton(sample,2)-0.5f, y=Halton(sample,3)-0.5f;
            applied=original;applied.m02-=2*x/renderWidth;applied.m12-=2*y/renderHeight;
            // Derive the SDK offset from the actual GPU projection, including Unity's RT Y flip.
            var gpuJittered=GL.GetGPUProjectionMatrix(applied,true);
            var anchor=new Vector4(0,0,-1,1);var plainClip=gpu*anchor;var jitteredClip=gpuJittered*anchor;
            float gpuX=(jitteredClip.x/jitteredClip.w-plainClip.x/plainClip.w)*renderWidth*0.5f;
            float gpuY=-(jitteredClip.y/jitteredClip.w-plainClip.y/plainClip.w)*renderHeight*0.5f;
            var toPrevious=previousVP*currentVP.inverse;
            // Keep the real previous projection through zoom/sprint animations.
            // Both DLAA and FG consume this mapping; resetting would make it identity.
            if(!reset && fovDelta>0.1f) {
                ++fovHistoryFrames;maxRetainedFovDelta=Math.Max(maxRetainedFovDelta,fovDelta);
                if(fovHistoryFrames<=3 || fovHistoryFrames%120==0)
                    log.Debug(string.Format(CultureInfo.InvariantCulture,
                        "HISTORY_FOV_CONTINUITY unityFrame={0} previousPreparedFrame={1} fovDeltaDegrees={2:F6} cutThresholdDegrees={3:F6} jitterSample={4} retainedFrames={5} previousMappingIdentity={6} cameraId={7}",
                        frame,previousFrame,fovDelta,DlaaHistoryPolicy.FovCutThreshold(elapsed),sample,fovHistoryFrames,Approximately(toPrevious,Matrix4x4.identity),camera.GetInstanceID()));
            }
            var p=camera.transform.position;var up=camera.transform.up;var right=camera.transform.right;var forward=camera.transform.forward;
            var packet=new NativeApi.CameraFrame {Size=(uint)Marshal.SizeOf<NativeApi.CameraFrame>(),Version=1,Frame=frame,Reset=reset?1:0,
                Projection=Rows(gpu),InverseProjection=Rows(gpu.inverse),ClipToPrevious=Rows(toPrevious),PreviousToClip=Rows(toPrevious.inverse),
                JitterX=gpuX,JitterY=gpuY,MotionScaleX=1,MotionScaleY=1,Near=camera.nearClipPlane,Far=camera.farClipPlane,Fov=camera.fieldOfView*Mathf.Deg2Rad,Aspect=camera.aspect,
                PosX=p.x,PosY=p.y,PosZ=p.z,UpX=up.x,UpY=up.y,UpZ=up.z,RightX=right.x,RightY=right.y,RightZ=right.z,
                ForwardX=forward.x,ForwardY=forward.y,ForwardZ=forward.z,ReversedDepth=SystemInfo.usesReversedZBuffer?1:0};
            if(NativeApi.UpscalerSubmitFrame(Context,ref packet)!=0) throw new InvalidOperationException("DLAA frame packet rejected");
            oldTransparentJitter=camera.useJitteredProjectionMatrixForTransparentRendering;
            camera.nonJitteredProjectionMatrix=original;
            camera.useJitteredProjectionMatrixForTransparentRendering=true;
            camera.projectionMatrix=applied;jittered=true;preparedFrame=frame;
            if(reset) {
                ++resetCount; ++resetRun; longestResetRun=Math.Max(longestResetRun,resetRun);
                log.Info(string.Format(CultureInfo.InvariantCulture,
                    "HISTORY_RESET unityFrame={0} previousPreparedFrame={1} reasons={2} elapsedSeconds={3:F6} positionDelta={4:F4} rotationDeltaDegrees={5:F4} fovDeltaDegrees={6:F6} fovDegrees={7:F4} previousFovDegrees={8:F4} resetCount={9} consecutive={10} longestRun={11} cameraId={12}",
                    frame,previousFrame,reasons.ToString().Replace(", ","|"),elapsed,distance,angle,fovDelta,camera.fieldOfView,previousFov,resetCount,resetRun,longestResetRun,camera.GetInstanceID()));
            } else resetRun=0;
            previousVP=currentVP;previousPosition=p;previousRotation=camera.transform.rotation;previousFov=camera.fieldOfView;previousTime=now;previousFrame=frame;
        }
        // The projection Unity derives from the camera's own properties. Anything else was
        // assigned by a script and is restored as-is; restoring an implicit one by assignment
        // would freeze it (the intro's physical camera animates its focal length).
        private Matrix4x4 ImplicitProjection()
        {
            if(!camera.usePhysicalProperties) return Matrix4x4.Perspective(camera.fieldOfView,camera.aspect,camera.nearClipPlane,camera.farClipPlane);
            Camera.CalculateProjectionMatrixFromPhysicalProperties(out var physical,camera.focalLength,camera.sensorSize,camera.lensShift,
                camera.nearClipPlane,camera.farClipPlane,new Camera.GateFitParameters(camera.gateFit,camera.aspect));
            return physical;
        }
        internal void RestoreProjection()
        {
            if(!jittered || !camera) {jittered=false;return;}
            if(Approximately(camera.projectionMatrix,applied)) {
                if(customProjection) camera.projectionMatrix=original; else camera.ResetProjectionMatrix();
            }
            if(Approximately(camera.nonJitteredProjectionMatrix,original)) camera.nonJitteredProjectionMatrix=originalNonJittered;
            if(camera.useJitteredProjectionMatrixForTransparentRendering) camera.useJitteredProjectionMatrixForTransparentRendering=oldTransparentJitter;
            jittered=false;
        }
        internal void Restore()
        {
            if(previousFrame!=0)log.Info("HISTORY_SUMMARY lastPreparedFrame="+previousFrame+" resets="+resetCount+" longestRun="+longestResetRun+
                " retainedFovFrames="+fovHistoryFrames+" maxRetainedFovDeltaDegrees="+maxRetainedFovDelta.ToString("F6",CultureInfo.InvariantCulture));
            // A stage change destroys the camera before this runs. Destroyed keys still
            // match by instance ID, so drop them too, or the static maps keep this session.
            if(!ReferenceEquals(layer,null) && owners.TryGetValue(layer,out var owner) && owner==this) {
                owners.Remove(layer);
                // Rebuild without the redirect before Output/PostOutput are released.
                if(Upscaled && layer) layer.OnGlobalVolumesDirty();
            }
            // A live layer drops its redirect when PPv2 next rebuilds; a destroyed one never does.
            if(!ReferenceEquals(layer,null) && !layer && redirected.TryGetValue(layer,out var post) && post==this) redirected.Remove(layer);
            Failure=null;preparedFrame=-1;
            RestoreProjection();
            if(changedAA && layer && layer.antialiasingMode==PostProcessLayer.Antialiasing.None) layer.antialiasingMode=oldAA;
            changedAA=false;
            foreach(var pair in scalers) if(pair.Key && !pair.Key.enabled) pair.Key.enabled=pair.Value;
            scalers.Clear();
            RestoreMipBias();RestoreParticleMotion();restored=true;
        }
        internal void Release()
        {
            if(Output){Output.Release();UnityEngine.Object.Destroy(Output);}
            if(PostOutput){PostOutput.Release();UnityEngine.Object.Destroy(PostOutput);}
            if(outlineMask){outlineMask.Release();UnityEngine.Object.Destroy(outlineMask);}
            if(outlineDisplayMask){outlineDisplayMask.Release();UnityEngine.Object.Destroy(outlineDisplayMask);}
            if(outlineSobel){outlineSobel.Release();UnityEngine.Object.Destroy(outlineSobel);}
            if(outlineComposite){outlineComposite.Release();UnityEngine.Object.Destroy(outlineComposite);}
            foreach(var texture in new[]{outlineOcclusion,outlineObjectDepth,outlineDisplaySobel}) if(texture){texture.Release();UnityEngine.Object.Destroy(texture);}
            outlineCommands?.Release();outlineCommands=null;
            compositeCommands?.Release();compositeCommands=null;
            fullResolutionCommands?.Release();fullResolutionCommands=null;
            unjitter?.Release();unjitter=null;
        }
        private static float Halton(int index,int radix) {float result=0,fraction=1;while(index>0){fraction/=radix;result+=fraction*(index%radix);index/=radix;}return result;}
        private static bool Approximately(Matrix4x4 a,Matrix4x4 b) {for(int i=0;i<16;++i)if(Math.Abs(a[i]-b[i])>0.00001f)return false;return true;}
        // Unity uses column-vector transforms. Transpose them for SL row-vector transforms.
        private static NativeApi.Matrix16 Rows(Matrix4x4 m) => new NativeApi.Matrix16 {
            A0=m.m00,A1=m.m10,A2=m.m20,A3=m.m30,B0=m.m01,B1=m.m11,B2=m.m21,B3=m.m31,
            C0=m.m02,C1=m.m12,C2=m.m22,C3=m.m32,D0=m.m03,D1=m.m13,D2=m.m23,D3=m.m33
        };
    }
}
