using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using HarmonyLib;
using UnityEngine;

namespace RiskOfRainUpscaling
{
    // Lower-resolution DLSS renders the scene camera into a smaller texture, so its
    // screen-space results are render pixels. These game methods hand the scene
    // camera's screen points to the full-size HUD camera (health bars, pings,
    // indicators, damage display, highlights, tooltips). Convert only that camera,
    // only while routing is active; every other camera keeps Unity's results.
    internal static class DisplaySpace
    {
        private static readonly (string type, string method)[] targets = {
            ("RoR2.UI.CombatHealthBarViewer", "UpdateAllHealthbarPositions"),
            ("RoR2.Indicator", "PositionForUI"),
            ("RoR2.PositionIndicator", "UpdatePositions"),
            ("RoR2.DamageDisplay", "OnUICameraPreCull"),
            ("RoR2.UI.HighlightRect", "WorldToGUIPoint"),
            ("RoR2.UI.PointViewer", "UpdateAllElementPositions"),
            ("RoR2.UI.TooltipController", "SetTooltip"),
        };
        private static readonly Dictionary<string, MethodInfo> replacements = new Dictionary<string, MethodInfo> {
            { "WorldToScreenPoint", AccessTools.Method(typeof(DisplaySpace), nameof(WorldToScreenPoint)) },
            { "ScreenToWorldPoint", AccessTools.Method(typeof(DisplaySpace), nameof(ScreenToWorldPoint)) },
            { "ViewportToScreenPoint", AccessTools.Method(typeof(DisplaySpace), nameof(ViewportToScreenPoint)) },
            { "ScreenToViewportPoint", AccessTools.Method(typeof(DisplaySpace), nameof(ScreenToViewportPoint)) },
            { "get_pixelRect", AccessTools.Method(typeof(DisplaySpace), nameof(PixelRect)) },
        };
        private static Camera routed;
        private static float scaleX = 1, scaleY = 1;
        private static bool patched;
        private static int replaced;

        // Returns a log line; never throws. Missing methods only lose that UI element's fix.
        internal static string Patch()
        {
            if (patched) return null;
            patched = true;
            var harmony = new Harmony("local.riskofrainupscaling.displayspace");
            var results = new List<string>();
            var assembly = typeof(RoR2.CameraRigController).Assembly;
            foreach (var (typeName, methodName) in targets)
            {
                try
                {
                    var type = assembly.GetType(typeName);
                    var methods = type == null ? new List<MethodInfo>() : AccessTools.GetDeclaredMethods(type).Where(m => m.Name == methodName).ToList();
                    if (methods.Count == 0) { results.Add(typeName + "." + methodName + "=missing"); continue; }
                    foreach (var method in methods)
                    {
                        replaced = 0;
                        harmony.Patch(method, transpiler: new HarmonyMethod(typeof(DisplaySpace), nameof(Transpiler)));
                        results.Add(typeName + "." + methodName + "=" + replaced);
                    }
                }
                catch (Exception exception) { results.Add(typeName + "." + methodName + "=failed:" + exception.GetBaseException().Message); }
            }
            return "DISPLAY_SPACE_PATCHED " + string.Join(" ", results);
        }
        internal static void Route(Camera scene, int renderWidth, int renderHeight, int outputWidth, int outputHeight)
        {
            routed = scene; scaleX = (float)outputWidth / renderWidth; scaleY = (float)outputHeight / renderHeight;
        }
        internal static void Clear(Camera scene)
        {
            if (routed == scene) { routed = null; scaleX = scaleY = 1; }
        }
        private static IEnumerable<CodeInstruction> Transpiler(IEnumerable<CodeInstruction> instructions)
        {
            foreach (var instruction in instructions)
            {
                if ((instruction.opcode == System.Reflection.Emit.OpCodes.Callvirt || instruction.opcode == System.Reflection.Emit.OpCodes.Call) &&
                    instruction.operand is MethodInfo method && method.DeclaringType == typeof(Camera) && !method.IsStatic &&
                    replacements.TryGetValue(method.Name, out var replacement) &&
                    method.GetParameters().Select(p => p.ParameterType).SequenceEqual(replacement.GetParameters().Skip(1).Select(p => p.ParameterType)))
                {
                    instruction.opcode = System.Reflection.Emit.OpCodes.Call; instruction.operand = replacement; ++replaced;
                }
                yield return instruction;
            }
        }
        private static bool Routed(Camera camera) => camera && routed && camera == routed;
        public static Vector3 WorldToScreenPoint(Camera camera, Vector3 position)
        {
            var point = camera.WorldToScreenPoint(position);
            if (Routed(camera)) { point.x *= scaleX; point.y *= scaleY; }
            return point;
        }
        public static Vector3 ScreenToWorldPoint(Camera camera, Vector3 position)
        {
            if (Routed(camera)) { position.x /= scaleX; position.y /= scaleY; }
            return camera.ScreenToWorldPoint(position);
        }
        public static Vector3 ViewportToScreenPoint(Camera camera, Vector3 position)
        {
            var point = camera.ViewportToScreenPoint(position);
            if (Routed(camera)) { point.x *= scaleX; point.y *= scaleY; }
            return point;
        }
        public static Vector3 ScreenToViewportPoint(Camera camera, Vector3 position)
        {
            if (Routed(camera)) { position.x /= scaleX; position.y /= scaleY; }
            return camera.ScreenToViewportPoint(position);
        }
        public static Rect PixelRect(Camera camera)
        {
            var rect = camera.pixelRect;
            return Routed(camera) ? new Rect(rect.x * scaleX, rect.y * scaleY, rect.width * scaleX, rect.height * scaleY) : rect;
        }
    }
}
