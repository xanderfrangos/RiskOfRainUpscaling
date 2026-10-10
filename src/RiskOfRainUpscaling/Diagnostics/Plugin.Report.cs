using System;
using System.Collections.Generic;
using System.Linq;
using BepInEx;
using RoR2;
using UnityEngine;
using UnityEngine.Rendering;

namespace RiskOfRainUpscaling
{
    // The startup environment and, at Debug, an inventory of cameras for bug reports (managed.log).
    public sealed partial class Plugin
    {
        private readonly Dictionary<int, string> cameraStates = new Dictionary<int, string>();
        private float nextScan;
        private string lastBuild;

        private void WriteEnvironment()
        {
            Write("Risk of Rain Upscaling " + Version + "; hotkeys enabled=" + settings.HotkeysEnabled.Value + "; toggle upscaling=" + settings.ToggleReconstruction.Value + "; toggle Frame Generation=" + settings.ToggleFrameGeneration.Value);
            Write("Unity=" + Application.unityVersion + "; game=" + Application.version + "; BepInEx=" + typeof(BaseUnityPlugin).Assembly.GetName().Version + "; CLR=" + Environment.Version + "; runtime=" + (Type.GetType("Mono.Runtime") != null ? "Mono" : "other"));
            Write("API=" + SystemInfo.graphicsDeviceType + "; GPU=" + SystemInfo.graphicsDeviceName + "; driver/API=" + SystemInfo.graphicsDeviceVersion + "; vendor=" + SystemInfo.graphicsDeviceVendor + "; memoryMiB=" + SystemInfo.graphicsMemorySize + "; OS=" + SystemInfo.operatingSystem);
            Write("Profile=" + Paths.BepInExRootPath + "; executable=" + Paths.ExecutablePath + "; resolution=" + Screen.width + "x" + Screen.height);
            Write("Pipeline=" + PipelineName());
        }
        private void TickReport()
        {
            if (Time.unscaledTime >= nextScan)
            {
                nextScan = Time.unscaledTime + 2f;
                if (log.IsEnabled(ModLogLevel.Debug)) ScanCameras();
                var build = RoR2Application.GetBuildId();
                if (build != lastBuild) { lastBuild = build; Write("RoR2 build=" + build); }
            }
        }
        private static string PipelineName()
        {
            var asset = GraphicsSettings.currentRenderPipeline;
            return asset ? asset.GetType().FullName : "Built-in (no active render-pipeline asset)";
        }
        private void ScanCameras()
        {
            var live = new HashSet<int>();
            foreach (var camera in Camera.allCameras)
            {
                int id = camera.GetInstanceID(); live.Add(id);
                var rig = CameraRigController.readOnlyInstancesList.FirstOrDefault(r => r && (r.sceneCam == camera || r.uiCam == camera || r.skyboxCam == camera));
                string role = !rig ? "unowned/preview/reflection" : rig.uiCam == camera ? "HUD" : rig.skyboxCam == camera ? "skybox" : CameraRole(camera,true) ?? "scene/spectator/cutscene/split-viewport";
                var components = string.Join(",", camera.GetComponents<Component>().Where(c => c && (c.GetType().FullName.IndexOf("PostProcess", StringComparison.OrdinalIgnoreCase) >= 0 || c.GetType().FullName.IndexOf("Render", StringComparison.OrdinalIgnoreCase) >= 0)).Select(c => c.GetType().FullName));
                string snapshot = "Camera id=" + id + "; name=" + camera.name + "; scene=" + camera.gameObject.scene.name + "; role=" + role + "; type=" + camera.cameraType + "; path=" + camera.actualRenderingPath + "; size=" + camera.pixelWidth + "x" + camera.pixelHeight + "; viewport=" + camera.rect + "; depth=" + camera.depth + "; cullingMask=" + camera.cullingMask + "; target=" + (camera.targetTexture ? camera.targetTexture.name : "display") + "; depthMode=" + camera.depthTextureMode + "; post/render=" + components;
                if (!cameraStates.TryGetValue(id, out var prior) || prior != snapshot) { Write(snapshot, ModLogLevel.Debug); cameraStates[id] = snapshot; }
            }
            foreach (var id in cameraStates.Keys.Where(id => !live.Contains(id)).ToArray()) { Write("Camera removed id=" + id, ModLogLevel.Debug); cameraStates.Remove(id); }
        }
        private void Write(string message, ModLogLevel severity = ModLogLevel.Info) => log.Write(severity, message);
    }
}
