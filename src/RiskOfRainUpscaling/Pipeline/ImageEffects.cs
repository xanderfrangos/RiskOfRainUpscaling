using System;
using System.Collections.Generic;
using System.Reflection;
using UnityEngine;

namespace RiskOfRainUpscaling
{
    // Whether a camera component is an image effect (declares OnRenderImage). The
    // pipeline asks about every scene and HUD camera component each frame, so the
    // reflection result is cached per type.
    internal static class ImageEffects
    {
        private static readonly Dictionary<Type, bool> types = new Dictionary<Type, bool>();

        internal static bool Is(MonoBehaviour component)
        {
            var type = component.GetType();
            if (!types.TryGetValue(type, out bool effect))
                types[type] = effect = type.GetMethod("OnRenderImage", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic) != null;
            return effect;
        }
    }
}
