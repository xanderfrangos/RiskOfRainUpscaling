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
    // Drives upscaling for the single eligible scene camera: attaches scene inputs,
    // world routing, the upscaler session and Frame Generation events, and falls back
    // safely. Partial files in Diagnostics/ add the debug overlay and capture.
    internal sealed partial class RenderPipeline : IDisposable
    {
        private readonly Plugin plugin;
        private readonly ModSettings settings;
        private readonly string folder;
        private readonly ModLog log;
        private readonly bool available;
        private readonly ConfigEntry<bool> overlay, captureWhileOff;
        internal const float MotionPreviewGain = 20f, DepthPreviewRange = 200f;
        private readonly ConfigEntry<bool> upscaling;
        private readonly ConfigEntry<DlssQuality> quality;
        private readonly ConfigEntry<UpscalerBackend> upscaler;
        private readonly ConfigEntry<DlssPreset> preset;
        private readonly ConfigEntry<OutlineMode> outlines;
        private int autoUpscaler;
        private readonly ConfigEntry<float> mipBiasOffset;
        // A scene effect that cannot run at display size moves upscaling to the
        // end of effects for the rest of the session.
        private DlaaPlacement placement = DlaaPlacement.BeforePostProcessing;
        private float nextMipScan, nextMotionScan;
        private readonly List<PipelineResources> retiring = new List<PipelineResources>();
        private PipelineResources active;
        private bool capturing, disposed, requestedCapture, attachFailed;
        private float nextAudit;
        private string lastBlocked;
        private int traceFrames;
        private readonly ConfigEntry<bool> frameGeneration, reflexLimiter, mainMenu;
        private readonly FrameGenerationSetting generation;
        private bool modEnabled=true;
        private readonly FrameGenerationTiming fgTiming;

        // The runtime the selected Upscaler resolves to, for sizing and the menu labels.
        internal UpscalerBackend ResolvedUpscaler => DlssPolicy.Resolve(upscaler.Value, autoUpscaler, SystemInfo.graphicsDeviceVendorID == DlssPolicy.NvidiaVendorId);

        internal RenderPipeline(Plugin plugin, ModSettings settings, ModLog log, string folder, bool native)
        {
            this.plugin = plugin; this.log = log; this.folder = folder; this.settings = settings;
            upscaling = settings.Upscaling;
            quality = settings.Quality;
            upscaler = settings.Upscaler;
            preset = settings.Preset;
            outlines = settings.Outlines;
            upscaling.SettingChanged += ReconstructionChanged;
            quality.SettingChanged += ReconstructionChanged;
            upscaler.SettingChanged += ReconstructionChanged;
            preset.SettingChanged += ReconstructionChanged;
            outlines.SettingChanged += ReconstructionChanged;
            mipBiasOffset = settings.MipBiasOffset;
            mainMenu = settings.MainMenu;
            frameGeneration = settings.FrameGeneration;
            generation = settings.Generation;
            reflexLimiter = settings.ReflexFrameLimiter;
            overlay = settings.ShowOverlay;
            captureWhileOff = settings.CaptureWhileOff;
            available = native && SystemInfo.graphicsDeviceType == GraphicsDeviceType.Direct3D11 && !GraphicsSettings.currentRenderPipeline && SystemInfo.SupportsRenderTextureFormat(RenderTextureFormat.RFloat) && SystemInfo.SupportsRenderTextureFormat(RenderTextureFormat.RGHalf);
            Log("PIPELINE_READY available=" + available + "; overlay=" + settings.ToggleOverlay.Value + "; cycle quality=" + settings.CycleQuality.Value + "; capture=" + settings.CaptureEvidence.Value + "; reconstruction=" + settings.ToggleReconstruction.Value + "; FG=" + settings.ToggleFrameGeneration.Value  + "; hotkeys enabled=" + settings.HotkeysEnabled.Value + "; quality=" + quality.Value +
                ". Color=end of effects, depth/motion=early. Lower-resolution world is composed into the HUD camera's target before the HUD draws.");
            Camera.onPreCull += BeforeCull; Camera.onPreRender += BeforeRender; Camera.onPostRender += AfterRender;
            if(available)fgTiming=new FrameGenerationTiming(()=>!disposed && modEnabled && plugin.isActiveAndEnabled && generation.LiveEnabled && upscaling.Value &&
                active?.Session!=null && active.Session.State>=1 && Time.timeScale>0 && Application.isFocused &&
                plugin.ReconstructionCamera(active.Scene,mainMenu.Value) && Screen.fullScreenMode!=FullScreenMode.ExclusiveFullScreen,
                ()=>!disposed && modEnabled && plugin.isActiveAndEnabled && settings.ReconstructionEnabled && reflexLimiter.Value,log);
        }
        private void Log(string message) => log.Info(message);
        private void LogDebug(string message) => log.Debug(message);
        private void LogWarning(string message) => log.Warning(message);
        private void LogError(string message) => log.Error(message);
        private bool HotkeyDown(ConfigEntry<KeyboardShortcut> shortcut) =>
            settings.HotkeysEnabled.Value && Application.isFocused && shortcut.Value.IsDown() && !plugin.IsBindingHotkey();
        internal bool OwnsTarget(Camera camera) { return active != null && active.Scene == camera && camera.targetTexture == active.World && active.World; }
        private void ReconstructionChanged(object sender, EventArgs args) { attachFailed = false; }
        internal void Tick(bool enabled)
        {
            modEnabled=enabled;
            CollectRetired();
            if (disposed) return;
            UpdateProfiler();
            if (HotkeyDown(settings.ToggleOverlay)) overlay.Value = !overlay.Value;
            if (HotkeyDown(settings.ToggleFrameGeneration)) {
                frameGeneration.Value=!frameGeneration.Value;
                Log("FRAME_GEN_REQUESTED "+frameGeneration.Value+(generation.RestartRequired?"; restart required to enable D3D12.":"; applies live; presentation runtime retained."));
            }
            if (HotkeyDown(settings.CycleQuality) && !capturing && upscaling.Value) {
                attachFailed=false;
                quality.Value=DlssPolicy.Next(quality.Value);Log("UPSCALER_QUALITY "+quality.Value);
            }
            if (HotkeyDown(settings.ToggleReconstruction) && !capturing) { upscaling.Value=!upscaling.Value; attachFailed=false; Log("UPSCALING_TOGGLED "+upscaling.Value); }
            if (HotkeyDown(settings.CaptureEvidence) && !capturing) requestedCapture = true;
            if (!modEnabled || (!captureWhileOff.Value && !upscaling.Value) || !available) { Detach("disabled/unsupported"); return; }
            // Finish the diagnostic readback before replacing its camera/resources.
            if (capturing) return;
            if(active?.Session!=null && active.Session.State<0) {upscaling.Value=false;active.Session.Restore();LogError("UPSCALING_FALLBACK native failure; Off selected; camera/AA restored; see native.log. The Toggle Upscaling shortcut retries explicitly.");}
            if (attachFailed) return;
            var rigs = CameraRigController.readOnlyInstancesList.Where(r => r && r.sceneCam && plugin.ReconstructionCamera(r.sceneCam,mainMenu.Value)).ToArray();
            if (rigs.Length != 1) { Detach("no single eligible gameplay/main-menu/character-select scene camera"); return; }
            var rig = rigs[0];
            if(rig.sceneCam.actualRenderingPath != RenderingPath.DeferredShading)
            { Detach("Scene inputs require the deferred scene path");return; }
            if (!rig.uiCam || !rig.uiCam.isActiveAndEnabled || rig.uiCam.targetTexture || rig.sceneCam.targetDisplay != 0 || rig.uiCam.targetDisplay != 0 ||
                rig.sceneCam.stereoEnabled || rig.uiCam.stereoEnabled || rig.uiCam.rect != new Rect(0,0,1,1) ||
                rig.uiCam.gameObject.scene!=rig.sceneCam.gameObject.scene || rig.uiCam.depth<=rig.sceneCam.depth)
            { Detach("HUD unavailable or belongs to another render target/display"); return; }
            bool dlaa = upscaling.Value;
            if(dlaa && (rig.sceneCam.orthographic || rig.sceneCam.usePhysicalProperties || QualitySettings.activeColorSpace!=ColorSpace.Linear))
            {upscaling.Value=false;LogError("UPSCALING_FALLBACK upscaling requires a perspective camera and Linear color space.");dlaa=false;}
            var dlss = dlaa ? quality.Value : DlssQuality.DLAA;
            bool low = dlaa && dlss != DlssQuality.DLAA;
            if (low && !WorldRoutingPolicy.CanRoute(true, true, rig.sceneCam.targetTexture && !OwnsTarget(rig.sceneCam), rig.uiCam.targetTexture,
                rig.skyboxCam && rig.skyboxCam.targetTexture && !(active != null && active.Sky == rig.skyboxCam && rig.skyboxCam.targetTexture == active.World),
                rig.sceneCam.depth, rig.uiCam.depth, rig.skyboxCam ? rig.skyboxCam.depth : 0, rig.skyboxCam && rig.skyboxCam.isActiveAndEnabled))
            { DisableLowerResolution(); Block("Unsupported camera ordering/foreign skybox target; lower-resolution rendering disabled (DLAA/native kept)."); return; }
            int width = Screen.width, height = Screen.height;
            if (width < 1 || height < 1) { Detach("display has no drawable pixels"); return; }
            // FSR's Balanced ratio differs from DLSS. A different Auto result rebuilds at its ratio.
            if (active?.Session != null && active.Session.Upscaler == UpscalerBackend.Auto && NativeApi.UpscalerKind(active.Session.Context) is int kind && kind > 0) autoUpscaler = kind;
            var backend = ResolvedUpscaler;
            int inputWidth = low ? DlssPolicy.Dimension(width, dlss, backend) : width;
            int inputHeight = low ? DlssPolicy.Dimension(height, dlss, backend) : height;
            if (active == null || active.Scene != rig.sceneCam || active.Hud != rig.uiCam || active.Sky != rig.skyboxCam ||
                active.SkyActive != (rig.skyboxCam && rig.skyboxCam.isActiveAndEnabled) || active.SceneOrder != rig.sceneCam.depth || active.HudOrder != rig.uiCam.depth ||
                active.Width != width || active.Height != height ||
                active.InputWidth != inputWidth || active.InputHeight != inputHeight || active.Low != low ||
                (active.Session!=null)!=dlaa || (active.Session!=null && (active.Session.Quality!=dlss || active.Session.Upscaler!=upscaler.Value || active.Session.Preset!=preset.Value || (active.Session.Upscaled && active.Session.Outlines!=outlines.Value) || active.Session.BeforePostProcessing!=DlssPolicy.BeforePostProcessing(dlss,EffectivePlacement(rig.sceneCam,dlss)))) ||
                active.Near != rig.sceneCam.nearClipPlane || active.Far != rig.sceneCam.farClipPlane)
            {
                Detach("camera/resolution/configuration changed");
                try { active = new PipelineResources(rig, width, height, inputWidth, inputHeight, low); Attach(dlss); }
                catch (Exception exception) { active?.Restore(); if (active != null) Retire(active); active = null; attachFailed = true;
                    upscaling.Value=false;
                    Block("Attach failed: " + exception.Message + "; renderer restored and paused. The Toggle Upscaling shortcut explicitly retries; restart also resets it. See native.log."); return; }
            }
            active.Scene.depthTextureMode |= DepthTextureMode.Depth | DepthTextureMode.MotionVectors;
            if ((active.Low || active.Session!=null) && !active.TailIsLast()) {
                DisableLowerResolution();
                upscaling.Value=false;
                Block("Another scene image effect was added after our final capture; DLAA/DLSS disabled."); return;
            }
            // Display-sized post-processing discards the render-sized OnRenderImage
            // chain; only effects we can rerun at display size are allowed there.
            if (active.Session?.Upscaled == true && active.Session.UnsupportedImageEffect(active.Tail) is string effect) {
                placement=DlaaPlacement.EndOfEffects;
                Block("Scene image effect " + effect + " cannot run at display size; upscaling moved to the end of effects for this session."); return;
            }
            if (NativeApi.SceneInputsState(active.Context) == -2) { upscaling.Value=false;attachFailed = true; Block("Native input capture failed; renderer restored. The Toggle Upscaling shortcut explicitly retries. See native.log."); return; }
            // Reflection over every camera event and component: only while Debug logging is on.
            if (log.IsEnabled(ModLogLevel.Debug) && Time.unscaledTime >= nextAudit)
            {
                nextAudit = Time.unscaledTime + 5f;
                AuditOrder(active);
            }
            // Pick up textures loaded after attach (stage loads, new items/enemies).
            if (active.Session != null && active.Session.State >= 0 && Time.unscaledTime >= nextMipScan)
            {
                nextMipScan = Time.unscaledTime + 10f;
                active.Session.ApplyMipBias();
            }
            // Effect prefabs load throughout a stage; catch them before many spawn.
            if (active.Session != null && active.Session.State >= 0 && Time.unscaledTime >= nextMotionScan)
            {
                nextMotionScan = Time.unscaledTime + 2f;
                active.Session.ApplyParticleMotion();
            }
            if (overlay.Value || requestedCapture) active.EnsureDiagnostics(LogWarning);
            if (active.Diagnostics != IntPtr.Zero && NativeApi.DiagnosticsState(active.Diagnostics) == -2 && !active.DiagnosticsFailureReported)
            {
                active.DiagnosticsFailureReported = true;
                LogWarning("Diagnostics failed; overlay previews and capture are unavailable for this camera. Upscaling is unaffected. See native.log.");
            }
            if (requestedCapture && !capturing)
            {
                requestedCapture = false;
                if (active.DiagnosticsReady) { capturing = true; plugin.RunCapture(Capture(active)); }
                else LogWarning("CAPTURE_FAILED diagnostics unavailable for this camera; see native.log.");
            }
        }
        // Upscaling before post-processing rebuilds the camera's PPv2 stack at display
        // size. A camera without an active layer keeps end-of-effects upscaling.
        private DlaaPlacement EffectivePlacement(Camera camera, DlssQuality dlss)
        {
            if (!DlssPolicy.UpscalesBeforePostProcessing(dlss, placement)) return placement;
            var layer = camera ? camera.GetComponent<PostProcessLayer>() : null;
            return layer && layer.isActiveAndEnabled ? placement : DlaaPlacement.EndOfEffects;
        }
        // Failure fallback for lower-resolution routing: keep reconstruction at 100%.
        // Profiling: camera-stage markers on the eligible rig (also while
        // upscaling is Off, for comparison) and the configuration for profile.log.
        private readonly ProfilerMarkers profilerMarkers = new ProfilerMarkers();
        private string profilerContext;
        private void UpdateProfiler()
        {
            if (!available || !Profiling.Enabled) { profilerMarkers.Remove(); profilerContext = null; return; }
            var rigs = CameraRigController.readOnlyInstancesList.Where(r => r && r.sceneCam && plugin.ReconstructionCamera(r.sceneCam, mainMenu.Value)).ToArray();
            var rig = rigs.Length == 1 ? rigs[0] : null;
            profilerMarkers.Update(modEnabled && rig, rig ? rig.sceneCam : null, rig ? rig.skyboxCam : null, rig ? rig.uiCam : null);
            var session = active?.Session;
            string context = session == null
                ? "upscaling=Off (pipeline " + (active == null ? "detached" : "attached without upscaling") + ") screen=" + Screen.width + "x" + Screen.height
                : "upscaling=" + session.Quality + " upscaler=" + NativeApi.UpscalerName(session.Context) + " dlssPreset=" + session.Preset + " world=" + active.InputWidth + "x" + active.InputHeight +
                  " output=" + active.Width + "x" + active.Height + " placement=" + (session.BeforePostProcessing ? "before post-processing" : "end of effects") +
                  " displayPost=" + session.PostRedirected + " sharpening=" + settings.Sharpness.Value.ToString("0.##") + " outlines=" + session.OutlinePath;
            context += " | FG=" + NativeApi.FrameGenerationLabel + " presentation=" + NativeApi.PresentationLabel + " targetFrameRate=" + Application.targetFrameRate + " vSyncCount=" + QualitySettings.vSyncCount;
            if (context == profilerContext) return;
            profilerContext = context; NativeApi.ProfilerSetContext(context);
        }
        private void DisableLowerResolution()
        {
            if (quality.Value != DlssQuality.DLAA) { quality.Value = DlssQuality.DLAA; Log("UPSCALER_QUALITY DLAA (lower-resolution routing failed)"); }
        }
        private void Block(string reason)
        {
            Detach(reason);
            if (lastBlocked != reason) { LogWarning(reason); lastBlocked = reason; }
        }
        private void Attach(DlssQuality dlss)
        {
            var resources = active;
            resources.Scene.depthTextureMode |= DepthTextureMode.Depth | DepthTextureMode.MotionVectors;
            resources.Geometry = new CommandBuffer { name = "RiskOfRainUpscaling.SceneInputs.AfterGBuffer" };
            resources.Geometry.IssuePluginEventAndData(NativeApi.SceneInputsGetEvent(), 90, resources.Context);
            resources.Scene.AddCommandBuffer(CameraEvent.AfterGBuffer, resources.Geometry);
            resources.Before = new CommandBuffer { name = "RiskOfRainUpscaling.SceneInputs.AfterForwardAlpha" };
            resources.Before.IssuePluginEventAndData(NativeApi.SceneInputsGetEvent(), 100, resources.Context);
            Profiling.Begin(resources.Before, "inputs.motionCopy (motion vectors -> RGHalf)");
            resources.Before.Blit(BuiltinRenderTextureType.MotionVectors, resources.Motion);
            // Stage 102 reads the blit's bound source, so it follows the blit directly.
            resources.Before.IssuePluginEventAndData(NativeApi.SceneInputsGetEvent(), 102, resources.Context);
            Profiling.End(resources.Before, "inputs.motionCopy (motion vectors -> RGHalf)");
            resources.Before.SetRenderTarget(BuiltinRenderTextureType.CameraTarget);
            resources.Scene.AddCommandBuffer(CameraEvent.AfterForwardAlpha, resources.Before);
            resources.After = new CommandBuffer { name = "RiskOfRainUpscaling.SceneInputs.SceneTail" };
            resources.After.Blit(BuiltinRenderTextureType.CameraTarget, resources.Color);
            resources.After.Blit(BuiltinRenderTextureType.CameraTarget, resources.HudFree);
            resources.After.SetRenderTarget(BuiltinRenderTextureType.CameraTarget);
            // Low-scale color comes from the explicit last OnRenderImage source.
            // Do not let a CameraTarget alias overwrite that copy later.
            if (!resources.Low && !upscaling.Value) resources.Scene.AddCommandBuffer(CameraEvent.AfterEverything, resources.After);
            // Route first: its world composition must precede the FG HUD-less copy
            // at the same HUD camera event.
            if (resources.Low) {
                var patched = DisplaySpace.Patch();
                if (patched != null) Log(patched);
                resources.Route();
            }
            resources.FgBefore=new CommandBuffer{name="RiskOfRainUpscaling.FrameGeneration.BeforeHud"};
            resources.Hud.AddCommandBuffer(CameraEvent.BeforeForwardOpaque,resources.FgBefore);
            resources.FgAfter=new CommandBuffer{name="RiskOfRainUpscaling.FrameGeneration.AfterHud"};
            resources.Hud.AddCommandBuffer(CameraEvent.BeforeImageEffects,resources.FgAfter);
            if(upscaling.Value) {
                resources.Session=new UpscalerSession(resources.Scene,resources.Context,resources.Color,resources.Depth,resources.Motion,
                    resources.Width,resources.Height,dlss,EffectivePlacement(resources.Scene,dlss),mipBiasOffset.Value,log,upscaler.Value,preset.Value,outlines.Value);
                resources.Session.Failure=e=>{upscaling.Value=false;LogError("UPSCALING_FALLBACK "+e.Message);};
                resources.Session.Initialize();
                if(resources.Session.BeforePostProcessing) {
                    // Added after our depth/motion capture at the same event, so the
                    // evaluation sees this frame's inputs before the post stack reads the scene.
                    resources.UpscalePre=new CommandBuffer{name="RiskOfRainUpscaling.Upscaling.BeforePostProcessing"};
                    resources.Scene.AddCommandBuffer(CameraEvent.AfterForwardAlpha,resources.UpscalePre);
                }
                nextMipScan = Time.unscaledTime + 10f; nextMotionScan = Time.unscaledTime + 2f;
                if(!resources.Tail) resources.Tail=resources.Scene.gameObject.AddComponent<SceneTail>();
                resources.Tail.Process=(source,destination)=>ProcessSceneTail(resources,source,destination);
            }
            traceFrames = 6; nextAudit = 0; lastBlocked = null;
            Log("PIPELINE_ATTACHED role="+(plugin.CameraRole(resources.Scene,mainMenu.Value)??"unknown")+" sceneName="+resources.Scene.gameObject.scene.name+" scene=" + resources.Scene.GetInstanceID() + " hud=" + resources.Hud.GetInstanceID() + " world=" + resources.InputWidth + "x" + resources.InputHeight +
                " output=" + resources.Width + "x" + resources.Height + " targetRouting=" + resources.Low + " quality=" + (resources.Session!=null ? dlss.ToString() : "native") + " near=" + resources.Near + " far=" + resources.Far +
                " reversedZ=" + SystemInfo.usesReversedZBuffer + "; depth/motion flags ORed.");
        }
        private RenderTexture ProcessSceneTail(PipelineResources resources, RenderTexture source, RenderTexture destination)
        {
            if(active!=resources || !source) return source;
            try {
                if(source.width!=resources.InputWidth || source.height!=resources.InputHeight) throw new InvalidOperationException("Final scene source does not match the world render size");
                if(resources.Session.Upscaled) return ProcessDisplayPost(resources,source,destination);
                var commands=resources.After;commands.Clear();commands.SetRenderTarget(source);
                Profiling.Begin(commands,"tail.copy scene -> SceneInput");
                commands.Blit(source,resources.Color);
                Profiling.End(commands,"tail.copy scene -> SceneInput");
                bool evaluate=resources.Session.Prepared;
                // At full resolution the before-HUD copy only feeds the overlay and capture.
                bool beforeHud=resources.Low || DiagnosticsWanted;
                if(evaluate) {
                    // Before post-processing, DLAA already ran on the HDR scene; only RCAS remains here.
                    commands.IssuePluginEventAndData(resources.Session.BeforePostProcessing?NativeApi.UpscalerGetSharpenEvent():NativeApi.UpscalerGetEvaluateEvent(),Time.frameCount,resources.Session.Context);
                    if(beforeHud) {Profiling.Begin(commands,"tail.copy output -> HudFree");commands.Blit(resources.Session.Output,resources.HudFree);Profiling.End(commands,"tail.copy output -> HudFree");}
                } else if(beforeHud) {Profiling.Begin(commands,"tail.copy scene -> HudFree");commands.Blit(source,resources.HudFree);Profiling.End(commands,"tail.copy scene -> HudFree");} // Bilinear when the world is lower resolution.
                commands.SetRenderTarget(destination?new RenderTargetIdentifier(destination):new RenderTargetIdentifier(BuiltinRenderTextureType.CameraTarget));
                Graphics.ExecuteCommandBuffer(commands);
                if (traceFrames > 0 && resources.Low)
                    LogDebug("SCENE_TAIL frame=" + Time.frameCount + " source=" + source.width + "x" + source.height + " evaluated=" + evaluate +
                        " destination=" + (destination ? destination.name + " " + destination.width + "x" + destination.height : "display"));
                // Lower resolution: HudFree reaches the display through the HUD camera.
                // The world target only needs the unscaled frame.
                return evaluate && !resources.Low?resources.Session.Output:source;
            } catch(Exception e) {
                upscaling.Value=false;resources.Session.Restore();LogError("UPSCALING_FALLBACK "+e.Message);return source;
            }
        }
        // Lower-resolution DLSS before post-processing. Source is the render-sized
        // scene after Unity's chain; the image we present was post-processed at
        // display size into PostOutput. Outlines/blur rerun on it, then RCAS.
        private RenderTexture ProcessDisplayPost(PipelineResources resources, RenderTexture source, RenderTexture destination)
        {
            var dlaa=resources.Session;
            var commands=resources.After;commands.Clear();commands.SetRenderTarget(source);
            Profiling.Begin(commands,"tail.copy scene -> SceneInput");
            commands.Blit(source,resources.Color);
            Profiling.End(commands,"tail.copy scene -> SceneInput");
            bool redirected=dlaa.PostRedirected;
            // Until PPv2 rebuilds its stack for us, source is still a normal
            // render-sized post-processed frame: bilinear, as when DLSS is unavailable.
            if(!redirected) commands.Blit(source,resources.HudFree);
            Graphics.ExecuteCommandBuffer(commands);commands.Clear();
            if(redirected) {
                var temporaries=new List<RenderTexture>();
                try {
                    Profiling.BeginNow("post.display-sized image effects (outline/blur rerun)");
                    var image=dlaa.RunDisplayEffects(resources.Tail,temporaries);
                    Profiling.EndNow("post.display-sized image effects (outline/blur rerun)");
                    Profiling.Begin(commands,"post.copy effects -> Output");
                    commands.Blit(image,dlaa.Output);
                    Profiling.End(commands,"post.copy effects -> Output");
                    commands.IssuePluginEventAndData(NativeApi.UpscalerGetSharpenEvent(),Time.frameCount,dlaa.Context);
                    Profiling.Begin(commands,"post.copy Output -> HudFree");
                    commands.Blit(dlaa.Output,resources.HudFree);
                    Profiling.End(commands,"post.copy Output -> HudFree");
                    commands.SetRenderTarget(destination?new RenderTargetIdentifier(destination):new RenderTargetIdentifier(BuiltinRenderTextureType.CameraTarget));
                    Graphics.ExecuteCommandBuffer(commands);
                }
                finally {foreach(var texture in temporaries)RenderTexture.ReleaseTemporary(texture);}
            }
            if (traceFrames > 0)
                LogDebug("SCENE_TAIL frame=" + Time.frameCount + " source=" + source.width + "x" + source.height + " displayPostProcessing=" + redirected + " prepared=" + dlaa.Prepared +
                    " destination=" + (destination ? destination.name + " " + destination.width + "x" + destination.height : "display"));
            // HudFree reaches the display through the HUD camera; the world target keeps the render-sized frame.
            return source;
        }
        private bool DiagnosticsWanted => overlay.Value || capturing;
        // Null when the HUD opacity replay is safe, otherwise why it is not (logged on change).
        private string HudOpacityUnsafe(Camera camera)
        {
            // The overlay is IMGUI drawn after the HUD camera, outside the HUD opacity replay.
            if(overlay.Value)return "diagnostic overlay";
            const System.Reflection.BindingFlags flags=System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic;
            foreach(var effect in camera.GetComponents<MonoBehaviour>().Where(c=>c && c.isActiveAndEnabled)) {
                var type=effect.GetType();
                if(type.GetMethod("OnRenderImage",flags)==null)continue;
                // DamageIndicator's zero-intensity branch resets its indicators
                // and blits unchanged color. A nonzero intensity or an unknown
                // image effect cannot safely share our pre-effect HUD opacity.
                if(type.FullName!="RoR2.PostProcessing.DamageIndicator")return "HUD image effect " + type.FullName;
                var intensity=type.GetField("_indicatorValue",flags);
                if(intensity==null || !(intensity.GetValue(effect) is float value) || !Mathf.Approximately(value,0))return "nonzero DamageIndicator";
            }
            // Screen-space overlay canvases render outside this camera's pass.
            foreach(var canvas in UnityEngine.Object.FindObjectsOfType<Canvas>())
                if(canvas.isActiveAndEnabled && canvas.renderMode==RenderMode.ScreenSpaceOverlay && canvas.targetDisplay==camera.targetDisplay)return "screen-space overlay canvas " + canvas.name;
            return null;
        }
        private string hudOpacityReason="";
        private void BeforeCull(Camera camera)
        {
            if (active == null || !camera) return;
            if(camera==active.Hud){
                var unsafeReason=HudOpacityUnsafe(camera);
                bool safe=unsafeReason==null;
                if(unsafeReason!=hudOpacityReason){
                    Log(safe?"FRAME_GEN_HUD_OPACITY safe; measured HUD opacity available":"FRAME_GEN_HUD_OPACITY unsafe reason=" + unsafeReason + "; the UI layer uses the opaque difference mask");
                    hudOpacityReason=unsafeReason;
                }
                active.FgBefore.Clear();active.FgAfter.Clear();
                active.FgBefore.IssuePluginEventAndData(NativeApi.FrameGenerationGetBeforeHudEvent(),Time.frameCount,safe?IntPtr.Zero:new IntPtr(1));
                active.FgAfter.IssuePluginEventAndData(NativeApi.FrameGenerationGetAfterHudEvent(),safe?Time.frameCount:-Time.frameCount,IntPtr.Zero);
            }
            if (active.Low && camera == active.Sky && camera.targetTexture != active.World)
            {
                DisableLowerResolution();
                Block("Skybox target was overridden before culling; lower-resolution mode disabled.");
                return;
            }
            if (camera == active.Scene)
            {
                if (active.Low && camera.targetTexture != active.World)
                {
                    DisableLowerResolution();
                    Block("Camera target was overridden before culling; lower-resolution mode disabled.");
                    return;
                }
                camera.depthTextureMode |= DepthTextureMode.Depth | DepthTextureMode.MotionVectors;
                // Event IDs are copied into Unity's queued command stream. A mutable
                // pointer containing a frame number would race the render thread.
                active.Geometry.Clear();
                active.Geometry.IssuePluginEventAndData(NativeApi.SceneInputsGetFrameEvent(), Time.frameCount, active.Context);
                active.Geometry.IssuePluginEventAndData(NativeApi.SceneInputsGetEvent(), 90, active.Context);
                active.RecordDiagnostics(DiagnosticsWanted);
                if(active.Session!=null && !active.Session.UsesPostProcessHook) {
                    try {active.Session.Prepare();}
                    catch(Exception e){upscaling.Value=false;active.Session.Restore();LogError("UPSCALING_FALLBACK "+e.Message);}
                }
                if (!capturing && traceFrames > 0) LogDebug("CAMERA_ORDER frame=" + Time.frameCount + " scene.preCull pixels=" + camera.pixelWidth + "x" + camera.pixelHeight + " target=" + (camera.targetTexture ? camera.targetTexture.name : "display"));
            }
        }
        private void BeforeRender(Camera camera)
        {
            if(active?.Session!=null && camera==active.Scene && !active.Session.ProjectionIntact) {
                upscaling.Value=false;active.Session.Restore();LogError("UPSCALING_FALLBACK projection overwritten after DLAA preparation");
            }
            // The event ID carries this frame's camera packet, so rebuild every
            // render. Unprepared frames (second renders, failures) leave the scene untouched.
            if(active?.UpscalePre!=null && camera==active.Scene) {
                active.UpscalePre.Clear();
                // The evaluation reads this frame's jittered outline inputs.
                Profiling.Begin(active.UpscalePre,"outlines.temporal inputs (mask + Sobel draws)");
                active.Session.RecordOutlineInputs(active.UpscalePre);
                Profiling.End(active.UpscalePre,"outlines.temporal inputs (mask + Sobel draws)");
                if(active.Session.Prepared) active.UpscalePre.IssuePluginEventAndData(NativeApi.UpscalerGetEvaluateEvent(),Time.frameCount,active.Session.Context);
                // Upscaling: the display-sized post stack reads the DLSS output every frame.
                else if(active.Session.Upscaled) active.UpscalePre.IssuePluginEventAndData(NativeApi.UpscalerGetFallbackEvent(),Time.frameCount,active.Session.Context);
            }
            if (active != null && camera == active.Scene) camera.depthTextureMode |= DepthTextureMode.Depth | DepthTextureMode.MotionVectors;
            if (capturing && active != null && camera == active.Scene && active.RequestFrame == Time.frameCount && active.CapturedScene == null)
            {
                active.CapturedScene = SceneMetadata(active);
            }
            if (capturing || active == null || traceFrames <= 0) return;
            if (camera == active.Scene || camera == active.Hud || camera == active.Sky)
                LogDebug("CAMERA_ORDER frame=" + Time.frameCount + " preRender=" + camera.name + " pixels=" + camera.pixelWidth + "x" + camera.pixelHeight + " depth=" + camera.depth);
        }
        private void AfterRender(Camera camera)
        {
            if (active == null || !camera) return;
            if(camera==active.Scene) active.Session?.RestoreProjection();
            if (active.Low && (camera == active.Scene || camera == active.Sky) && camera.targetTexture != active.World)
            { DisableLowerResolution(); Block("Owned world target overridden during render; routing disabled."); return; }
            if (capturing || traceFrames <= 0) return;
            if (camera == active.Scene || camera == active.Hud || camera == active.Sky)
                LogDebug("CAMERA_ORDER frame=" + Time.frameCount + " postRender=" + camera.name);
            if (camera == active.Hud) --traceFrames;
        }
        private void AuditOrder(PipelineResources resources)
        {
            foreach (var camera in new[] { resources.Scene, resources.Sky, resources.Hud }.Where(c => c))
            {
                string commands = string.Join("; ", Enum.GetValues(typeof(CameraEvent)).Cast<CameraEvent>().Select(e => e + ":" +
                    string.Join(",", camera.GetCommandBuffers(e).Select(b => b.name))).Where(s => !s.EndsWith(":")));
                string callbacks = string.Join(",", camera.GetComponents<MonoBehaviour>().Where(c => c).SelectMany(c =>
                    new[] { "OnPreCull", "OnPreRender", "OnPostRender", "OnRenderImage" }.Where(name => c.GetType().GetMethod(name,
                        System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic) != null)
                    .Select(name => c.GetType().FullName + "." + name)));
                var layer = camera.GetComponent<PostProcessLayer>();
                string state = camera.GetInstanceID() + "|" + commands + "|" + callbacks + "|AA=" + (layer ? layer.antialiasingMode.ToString() : "none");
                if (!resources.OrderStates.Add(state)) continue;
                LogDebug("CAMERA_COMMANDS camera=" + camera.name + " components=" + callbacks + " commandBuffers=" + commands);
                if (layer) {
                    try {
                        var blur = layer.GetSettings<MotionBlur>();
                        LogDebug("POST_PROCESS camera=" + camera.name + " active=" + layer.isActiveAndEnabled + " antialiasing=" + layer.antialiasingMode +
                            " motionBlurSetting=" + (blur ? (blur.active && blur.enabled.value).ToString() : "unavailable") + " shutterAngle=" + (blur ? blur.shutterAngle.value.ToString() : "unavailable"));
                    } catch(Exception exception) { LogDebug("POST_PROCESS settings unavailable: " + exception.Message); }
                }
            }
        }
        private void Detach(string reason)
        {
            if (active == null) return;
            var resources = active; active = null;
            resources.Restore(); Retire(resources);
            Log("PIPELINE_DETACHED reason=" + reason + "; owned targets/clear flags restored; depth/motion request flags retained because other effects can share them.");
        }
        private void Retire(PipelineResources resources)
        {
            var commands = new CommandBuffer { name = "RiskOfRainUpscaling.Pipeline.Retire" };
            try {
                if(resources.Session!=null)commands.IssuePluginEventAndData(NativeApi.UpscalerGetEvaluateEvent(),-1,resources.Session.Context);
                if(resources.Diagnostics!=IntPtr.Zero)commands.IssuePluginEventAndData(NativeApi.DiagnosticsGetEvent(),-1,resources.Diagnostics);
                commands.IssuePluginEventAndData(NativeApi.SceneInputsGetEvent(), 199, resources.Context); Graphics.ExecuteCommandBuffer(commands);
            }
            finally { commands.Release(); }
            retiring.Add(resources);
        }
        private void CollectRetired()
        {
            foreach (var resources in retiring.ToArray())
            {
                if (NativeApi.SceneInputsState(resources.Context) != -1) continue;
                if(resources.Session!=null) {if(resources.Session.State!=-1)continue;NativeApi.UpscalerDestroy(resources.Session.Context);resources.Session.Context=IntPtr.Zero;}
                if(resources.Diagnostics!=IntPtr.Zero) {if(NativeApi.DiagnosticsState(resources.Diagnostics)!=-1)continue;NativeApi.DiagnosticsDestroy(resources.Diagnostics);resources.Diagnostics=IntPtr.Zero;}
                NativeApi.SceneInputsDestroy(resources.Context); resources.Context = IntPtr.Zero; resources.Release(); retiring.Remove(resources);
            }
        }
        public void Dispose()
        {
            upscaling.SettingChanged -= ReconstructionChanged;
            quality.SettingChanged -= ReconstructionChanged;
            upscaler.SettingChanged -= ReconstructionChanged;
            preset.SettingChanged -= ReconstructionChanged;
            outlines.SettingChanged -= ReconstructionChanged;
            disposed = true; Camera.onPreCull -= BeforeCull; Camera.onPreRender -= BeforeRender; Camera.onPostRender -= AfterRender;
            fgTiming?.Dispose();
            profilerMarkers.Remove();
            Detach("plugin shutdown"); CollectRetired();
            // A callback still in Unity's queue owns this context and these RTs.
            // Keep them rooted until process exit rather than freeing queued data.
            PipelineResources.RetainedOnExit.AddRange(retiring); retiring.Clear();
        }
    }
}
