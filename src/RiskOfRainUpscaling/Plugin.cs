using System;
using System.IO;
using System.Linq;
using System.Globalization;
using BepInEx;
using BepInEx.Bootstrap;
using RoR2;
using UnityEngine;
using Path = System.IO.Path;

namespace RiskOfRainUpscaling
{
    [BepInPlugin(Guid, "Risk of Rain Upscaling", Version)]
    [BepInDependency(RiskOfOptionsIntegration.Guid, BepInDependency.DependencyFlags.SoftDependency)]
    public sealed partial class Plugin : BaseUnityPlugin
    {
        internal const string Guid = "xanderfrangos.riskofrainupscaling";
        // Version is generated from Directory.Build.props by RiskOfRainUpscaling.csproj.
        private ModSettings settings;
        private ModLog log;
        private volatile bool loggingPending = true;
        internal Func<bool> IsBindingHotkey = () => false;
        private bool sharpnessPending = true;
        private volatile bool frameGenerationMultiplierPending = true;
        private volatile bool frameGenerationRestartPrompt;
        private string logFolder;
        private bool nativeLoaded;
        private RenderPipeline pipeline;

        private void Awake()
        {
            settings = new ModSettings(Config);
            settings.Generation.SetPresentationState(0);
            settings.Sharpness.SettingChanged += SharpnessChanged;
            settings.Generation.Multiplier.SettingChanged += FrameGenerationMultiplierChanged;
            settings.Generation.Requested.SettingChanged += FrameGenerationEnabledChanged;
            settings.LoggingLevel.SettingChanged += LoggingChanged;
            logFolder = Path.Combine(Paths.BepInExRootPath, "RiskOfRainUpscaling-Diagnostics");
            Directory.CreateDirectory(logFolder);
            log = new ModLog(settings.Logging.Allows, (level, message) => settings.Logging.Write(Logger, level, message), Path.Combine(logFolder, "managed.log"));
            if (Chainloader.PluginInfos.ContainsKey(RiskOfOptionsIntegration.Guid))
            {
                try
                {
                    var frameRate = new GameFrameRateSetting(
                        () => RoR2.Console.instance?.FindConVar("fps_max")?.GetString() ?? Application.targetFrameRate.ToString(CultureInfo.InvariantCulture),
                        value => RoR2.Console.instance.FindConVar("fps_max").SetString(value));
                    UpscalerBackend Resolved() => pipeline?.ResolvedUpscaler ?? DlssPolicy.Resolve(settings.Upscaler.Value, 0, SystemInfo.graphicsDeviceVendorID == DlssPolicy.NvidiaVendorId);
                    RiskOfOptionsIntegration.Register(settings, frameRate,
                        () => settings.Upscaling.Value && Resolved() == UpscalerBackend.DLSS,
                        message => LogMessage(ModLogLevel.Warning, message));
                    IsBindingHotkey = RiskOfOptionsIntegration.IsBindingHotkey;
                }
                catch (Exception ex) { LogMessage(ModLogLevel.Warning, "Risk of Options registration failed: " + ex.Message); }
            }
            if (!settings.Enabled.Value) { LogMessage(ModLogLevel.Info, "Mod disabled by General.Enabled."); return; }
            WriteEnvironment();
            try
            {
                var path = PackagePaths.FindFile(Paths.PatcherPluginPath, "RiskOfRainUpscaling.Native.dll");
                NativeApi.Load(path);
                NativeApi.SetLoggingLevel((int)settings.LoggingLevel.Value);
                var startup = NativeApi.Initialize(logFolder, Path.Combine(Path.GetDirectoryName(Info.Location), "runtimes"), "plugin-awake");
                if (startup < 0)
                    LogMessage(ModLogLevel.Warning, "Early graphics hooks missed; Frame Generation stays OFF until restart. Check the preloader installation and TraceGraphicsStartup setting. See native.log.");
                nativeLoaded = true;
                settings.Generation.SetPresentationState(NativeApi.GetPresentationState());
                Write("Presentation="+NativeApi.PresentationLabel+"; Frame Generation="+NativeApi.FrameGenerationLabel+"; startup runtime selection in "+Config.ConfigFilePath);
                Write("Native library ready; observed D3D11 devices=" + NativeApi.GetObservedDeviceCount() + " (0 means the preloader hooks missed startup).");
            }
            catch (Exception ex) { Write("Native library unavailable: " + ex.Message, ModLogLevel.Error); }
            pipeline = new RenderPipeline(this, settings, log, logFolder, nativeLoaded);
            if (log.IsEnabled(ModLogLevel.Debug)) ScanCameras();
        }

        private void Update()
        {
            ApplyLiveSettings();
            if (frameGenerationRestartPrompt && RoR2Application.loadFinished && Application.isFocused)
            {
                frameGenerationRestartPrompt = false;
                if (settings.Generation.RestartRequired)
                {
                    var dialog = RoR2.UI.SimpleDialogBox.Create(null);
                    dialog.headerToken = new RoR2.UI.SimpleDialogBox.TokenParamsPair { token = "Restart required", formatParams = Array.Empty<object>() };
                    dialog.descriptionToken = new RoR2.UI.SimpleDialogBox.TokenParamsPair {
                        token = "Frame Generation is saved as enabled. Restart Risk of Rain 2 to start the required D3D12 presentation runtime.", formatParams = Array.Empty<object>() };
                    dialog.AddCancelButton("OK");
                }
            }
            pipeline?.Tick(settings.Enabled.Value);
            if (!settings.Enabled.Value) return;
            TickReport();
        }

        internal bool Gameplay(Camera camera)
        {
            foreach (var rig in CameraRigController.readOnlyInstancesList)
            {
                if (!rig || rig.sceneCam != camera) continue;
                var user = rig.localUserViewer;
                bool ownBody = user != null && user.cachedBody && user.cachedBody.healthComponent &&
                    user.cachedBody.healthComponent.alive && user.cachedBody == rig.targetBody;
                var rect = camera.rect;
                return CameraPolicy.IsGameplay(true, user != null, ownBody, rig.isCutscene,
                    camera.targetTexture && !(pipeline?.OwnsTarget(camera) ?? false), Mathf.Approximately(rect.x, 0) && Mathf.Approximately(rect.y, 0) &&
                    Mathf.Approximately(rect.width, 1) && Mathf.Approximately(rect.height, 1),
                    camera.isActiveAndEnabled, camera.cameraType == CameraType.Game);
            }
            return false;
        }

        internal bool ReconstructionCamera(Camera camera, bool menus) => CameraRole(camera,menus)!=null;

        // Local gameplay, the title menu, character select or the intro cutscene; null for any other camera.
        internal string CameraRole(Camera camera, bool menus)
        {
            if(!camera)return null;
            if(Gameplay(camera))return "local-gameplay";
            var rect=camera.rect;
            var scene=camera.gameObject.scene;
            bool sceneCamera=CameraRigController.readOnlyInstancesList.Any(r=>r && r.sceneCam==camera);
            bool runActive=Run.instance, renderTexture=camera.targetTexture && !(pipeline?.OwnsTarget(camera)??false),
                fullViewport=Mathf.Approximately(rect.x,0) && Mathf.Approximately(rect.y,0) &&
                    Mathf.Approximately(rect.width,1) && Mathf.Approximately(rect.height,1),
                active=camera.isActiveAndEnabled, gameCamera=camera.cameraType==CameraType.Game;
            var menu=RoR2.UI.MainMenu.MainMenuController.instance;
            if(CameraPolicy.IsMainMenu(sceneCamera,menus,scene.name=="title",
                menu && menu.isActiveAndEnabled && menu.gameObject.scene==scene,
                runActive,renderTexture,fullViewport,active,gameCamera))
                return "main-menu-scene";
            var lobby=PreGameController.instance;
            if(CameraPolicy.IsCharacterSelect(sceneCamera,menus,lobby && lobby.isActiveAndEnabled && lobby.gameObject.scene==scene,
                runActive,renderTexture,fullViewport,active,gameCamera))
                return "character-select-scene";
            if(CameraPolicy.IsIntro(sceneCamera,menus,scene.name=="intro",IntroController(scene),
                runActive,renderTexture,fullViewport,active,gameCamera))
                return "intro-cutscene";
            return null;
        }

        // IntroCutsceneController is not a singleton; search once per intro scene load.
        private IntroCutsceneController intro;
        private int introSearchedScene;
        private bool IntroController(UnityEngine.SceneManagement.Scene scene)
        {
            if(scene.name!="intro")return false;
            if(introSearchedScene!=scene.handle || (!intro && Time.frameCount%60==0)) {
                introSearchedScene=scene.handle;
                intro=FindObjectsOfType<IntroCutsceneController>().FirstOrDefault(c=>c.gameObject.scene==scene);
            }
            return intro && intro.isActiveAndEnabled && intro.gameObject.scene==scene;
        }

        internal void LogMessage(ModLogLevel severity, string message) => log.Write(severity, message);
        private void OnDestroy()
        {
            if (settings != null)
            {
                settings.Sharpness.SettingChanged -= SharpnessChanged;
                settings.Generation.Multiplier.SettingChanged -= FrameGenerationMultiplierChanged;
                settings.Generation.Requested.SettingChanged -= FrameGenerationEnabledChanged;
                settings.LoggingLevel.SettingChanged -= LoggingChanged;
            }
            pipeline?.Dispose();
            log?.Dispose();
        }
        private void OnGUI() { pipeline?.DrawOverlay(); }
        internal void RunCapture(System.Collections.IEnumerator routine) { StartCoroutine(routine); }
        internal void PromptFrameGenerationRestart() { frameGenerationRestartPrompt = true; }

        private void SharpnessChanged(object sender, EventArgs args) { sharpnessPending = true; }
        private void LoggingChanged(object sender, EventArgs args) { loggingPending = true; }
        private void FrameGenerationMultiplierChanged(object sender, EventArgs args) { frameGenerationMultiplierPending = true; }
        private void FrameGenerationEnabledChanged(object sender, EventArgs args)
        {
            frameGenerationMultiplierPending = true;
            PromptFrameGenerationRestart();
        }
        private void ApplyLiveSettings()
        {
            if (nativeLoaded)
            {
                if (loggingPending)
                {
                    loggingPending = false;
                    NativeApi.SetLoggingLevel((int)settings.LoggingLevel.Value);
                }
                bool previous = settings.Generation.RuntimeAvailable;
                settings.Generation.SetPresentationState(NativeApi.GetPresentationState());
                if (previous != settings.Generation.RuntimeAvailable) frameGenerationMultiplierPending = true;
            }
            // SettingChanged can also come from a config reload. Native and game
            // calls stay on Unity's main thread, with RCAS applied on the render thread.
            // Cost profiler.
            if (nativeLoaded && Profiling.Enabled != (settings.Profiling.Value && settings.Enabled.Value))
            {
                Profiling.Enabled = settings.Profiling.Value && settings.Enabled.Value;
                NativeApi.ProfilerSetEnabled(Profiling.Enabled ? 1 : 0);
                LogMessage(ModLogLevel.Info, "PROFILING " + (Profiling.Enabled ? "on; see profile.log" : "off"));
            }
            if (sharpnessPending && nativeLoaded)
            {
                sharpnessPending = false;
                NativeApi.SetSharpness(settings.Sharpness.Value);
            }
            if (frameGenerationMultiplierPending && nativeLoaded)
            {
                frameGenerationMultiplierPending = false;
                int multiplier = settings.Generation.LiveMultiplier;
                if (multiplier != 0) NativeApi.FrameGenerationSetMultiplier(multiplier);
                else NativeApi.FrameGenerationSetEnabled(0);
            }
        }
    }
}

