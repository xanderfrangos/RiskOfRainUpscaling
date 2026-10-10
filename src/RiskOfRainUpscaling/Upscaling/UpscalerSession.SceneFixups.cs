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
    // Scene fixups while upscaling: texture mip bias and camera-motion vectors for particles.
    internal sealed partial class UpscalerSession
    {
        // Built-in Unity has no global LOD bias, so bias each mipmapped texture
        // and remember the original value. Textures loaded later are covered by
        // the periodic rescan from RenderPipeline.
        private readonly Dictionary<Texture2D,float> biased=new Dictionary<Texture2D,float>();
        private bool restored;
        internal void ApplyMipBias()
        {
            if(mipBias==0 || restored)return;
            var watch=System.Diagnostics.Stopwatch.StartNew();
            int added=0;
            foreach(var texture in UnityEngine.Resources.FindObjectsOfTypeAll<Texture2D>())
            {
                if(!texture || texture.mipmapCount<=1 || biased.ContainsKey(texture))continue;
                biased.Add(texture,texture.mipMapBias);texture.mipMapBias+=mipBias;++added;
            }
            if(added>0)log.Debug("MIP_BIAS added="+added+" total="+biased.Count+" bias="+mipBias.ToString("F3",CultureInfo.InvariantCulture)+" scanMs="+watch.ElapsedMilliseconds);
        }
        // Hopoo particle shaders have no MotionVectors pass, and Unity's per-object
        // fallback writes garbage for world-space particle geometry (thousands of
        // pixels, e.g. GenericFootstepDust). Camera motion
        // reprojects their depth instead, matching world-anchored particles.
        // FindObjectsOfTypeAll includes loaded prefabs, so new clones inherit it.
        private readonly Dictionary<Renderer,MotionVectorGenerationMode> particleMotion=new Dictionary<Renderer,MotionVectorGenerationMode>();
        internal void ApplyParticleMotion()
        {
            if(restored)return;
            int added=0;
            foreach(var renderer in UnityEngine.Resources.FindObjectsOfTypeAll<ParticleSystemRenderer>().Cast<Renderer>()
                .Concat(UnityEngine.Resources.FindObjectsOfTypeAll<TrailRenderer>()).Concat(UnityEngine.Resources.FindObjectsOfTypeAll<LineRenderer>()))
            {
                if(!renderer || renderer.motionVectorGenerationMode!=MotionVectorGenerationMode.Object || particleMotion.ContainsKey(renderer))continue;
                if(renderer.sharedMaterials.Any(m=>m && Enumerable.Range(0,m.passCount).Any(i=>string.Equals(m.GetPassName(i),"MotionVectors",StringComparison.OrdinalIgnoreCase))))continue;
                particleMotion.Add(renderer,renderer.motionVectorGenerationMode);
                renderer.motionVectorGenerationMode=MotionVectorGenerationMode.Camera;++added;
            }
            if(added>0)log.Debug("PARTICLE_MOTION camera-motion added="+added+" total="+particleMotion.Count+"; particle/trail/line renderers without a MotionVectors pass");
        }
        private void RestoreParticleMotion()
        {
            foreach(var pair in particleMotion)
                if(pair.Key && pair.Key.motionVectorGenerationMode==MotionVectorGenerationMode.Camera)pair.Key.motionVectorGenerationMode=pair.Value;
            particleMotion.Clear();
        }
        private void RestoreMipBias()
        {
            foreach(var pair in biased)
                if(pair.Key && Math.Abs(pair.Key.mipMapBias-(pair.Value+mipBias))<0.0001f)pair.Key.mipMapBias=pair.Value;
            biased.Clear();
        }
    }
}
