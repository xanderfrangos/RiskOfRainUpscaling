using System;
using System.Collections.Generic;
using System.Linq;
using UnityEngine;
using UnityEngine.Rendering;

namespace RiskOfRainUpscaling
{
    // Cost profiler ([Diagnostics] Profiling). Unity-side steps are
    // bracketed by native GPU timestamps through a render event; the native side
    // groups them per frame and writes profile.log. Events recorded into command
    // buffers are no-ops while profiling is off.
    internal static class Profiling
    {
        private const int BeginKind = 0, EndKind = 1, InstantKind = 2;
        private static readonly Dictionary<string, int> labels = new Dictionary<string, int>();
        private static CommandBuffer immediate;
        // Set on the main thread when the setting is applied.
        internal static bool Enabled;
        private static bool Available => NativeApi.ProfilerGetEvent != null && NativeApi.ProfilerRegister != null;

        private static int Label(string name)
        {
            if (!labels.TryGetValue(name, out int id)) labels[name] = id = NativeApi.ProfilerRegister(name);
            return id;
        }
        private static void Issue(CommandBuffer commands, string label, int kind)
        {
            if (!Available || commands == null) return;
            int id = Label(label);
            if (id >= 0) commands.IssuePluginEventAndData(NativeApi.ProfilerGetEvent(), id << 2 | kind, IntPtr.Zero);
        }
        internal static void Begin(CommandBuffer commands, string label) => Issue(commands, label, BeginKind);
        internal static void End(CommandBuffer commands, string label) => Issue(commands, label, EndKind);
        internal static void Mark(CommandBuffer commands, string label) => Issue(commands, label, InstantKind);
        // For immediate work (Graphics.Blit in OnRenderImage), only while profiling.
        internal static void BeginNow(string label) => Now(label, BeginKind);
        internal static void EndNow(string label) => Now(label, EndKind);
        private static void Now(string label, int kind)
        {
            if (!Enabled || !Available) return;
            immediate = immediate ?? new CommandBuffer { name = "RiskOfRainUpscaling.Profiler" };
            immediate.Clear(); Issue(immediate, label, kind); Graphics.ExecuteCommandBuffer(immediate);
        }
    }

    // A timestamp at every camera event of the scene, skybox and HUD
    // cameras, so the log shows each Unity stage (G-buffer, lighting, motion
    // vectors, post-processing...) with and without upscaling. Each marker is
    // inserted before the other buffers at its event.
    internal sealed class ProfilerMarkers
    {
        private readonly List<(Camera camera, CameraEvent stage, CommandBuffer commands)> attached = new List<(Camera, CameraEvent, CommandBuffer)>();
        private Camera scene, sky, hud;

        internal void Update(bool enabled, Camera sceneCamera, Camera skyCamera, Camera hudCamera)
        {
            if (!enabled || !sceneCamera) { Remove(); return; }
            if (attached.Count > 0 && scene == sceneCamera && sky == skyCamera && hud == hudCamera && attached.All(a => a.camera)) return;
            Remove();
            scene = sceneCamera; sky = skyCamera; hud = hudCamera;
            Attach(sceneCamera, "scene"); Attach(skyCamera, "sky"); Attach(hudCamera, "hud");
        }
        private void Attach(Camera camera, string role)
        {
            if (!camera) return;
            foreach (CameraEvent stage in Enum.GetValues(typeof(CameraEvent)))
            {
                var commands = new CommandBuffer { name = "RiskOfRainUpscaling.Profiler." + role + "." + stage };
                Profiling.Mark(commands, role + ":" + stage);
                var others = camera.GetCommandBuffers(stage);
                foreach (var other in others) camera.RemoveCommandBuffer(stage, other);
                camera.AddCommandBuffer(stage, commands);
                foreach (var other in others) camera.AddCommandBuffer(stage, other);
                attached.Add((camera, stage, commands));
            }
        }
        internal void Remove()
        {
            foreach (var (camera, stage, commands) in attached)
            {
                if (camera) camera.RemoveCommandBuffer(stage, commands);
                commands.Release();
            }
            attached.Clear(); scene = sky = hud = null;
        }
    }
}
