using System;
using UnityEngine;

namespace RiskOfRainUpscaling
{
    // Added after the game's image effects: source is their explicit final output.
    public sealed class SceneTail : MonoBehaviour
    {
        internal Func<RenderTexture, RenderTexture, RenderTexture> Process;

        private void OnRenderImage(RenderTexture source, RenderTexture destination)
        {
            var presented = source;
            try { presented = Process?.Invoke(source, destination) ?? source; }
            finally {
                Profiling.BeginNow("tail.final blit to the camera destination");
                Graphics.Blit(presented, destination);
                Profiling.EndNow("tail.final blit to the camera destination");
            }
        }
    }
}
