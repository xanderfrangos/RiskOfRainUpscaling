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
    // Lower-resolution DLSS before post-processing: the post stack and image effects at display size.
    internal sealed partial class UpscalerSession
    {
        // ---- Lower-resolution DLSS before post-processing ----
        // The scene camera still renders its world at render size. DLSS upscales the
        // HDR scene after transparents into Output; the post-transparency PPv2 stack
        // (the "Post-processing" BeforeImageEffects buffer) is rebuilt with a
        // display-sized context reading Output and writing PostOutput. The game's
        // OnRenderImage effects then run on the render-sized scene, which we no longer
        // present, so the supported ones are skipped there and rerun at display size
        // from our tail. Opaque-only effects keep running at render size before DLSS.
        private static bool postPatched;
        private static AccessTools.FieldRef<PostProcessLayer,CommandBuffer> legacyPostBuffer;
        private static AccessTools.FieldRef<PostProcessRenderContext,RenderTextureDescriptor> sourceDescriptor;
        private static MethodInfo setWidth, setHeight, setScreenWidth, setScreenHeight, outlineImage, blurImage;
        // Which owner the layer's cached post buffer was last built for.
        private static readonly Dictionary<PostProcessLayer,UpscalerSession> redirected=new Dictionary<PostProcessLayer,UpscalerSession>();
        private static bool displayEffects;
        private CommandBuffer unjitter;
        private bool loggedRedirect;
        private static void PatchDisplayPostProcessing()
        {
            if(postPatched)return;
            var render=AccessTools.DeclaredMethod(typeof(PostProcessLayer),"Render",new[]{typeof(PostProcessRenderContext),typeof(bool)});
            legacyPostBuffer=AccessTools.FieldRefAccess<PostProcessLayer,CommandBuffer>("m_LegacyCmdBuffer");
            sourceDescriptor=AccessTools.FieldRefAccess<PostProcessRenderContext,RenderTextureDescriptor>("m_sourceDescriptor");
            setWidth=AccessTools.PropertySetter(typeof(PostProcessRenderContext),"width");
            setHeight=AccessTools.PropertySetter(typeof(PostProcessRenderContext),"height");
            setScreenWidth=AccessTools.PropertySetter(typeof(PostProcessRenderContext),"screenWidth");
            setScreenHeight=AccessTools.PropertySetter(typeof(PostProcessRenderContext),"screenHeight");
            outlineImage=AccessTools.DeclaredMethod(typeof(OutlineHighlight),"OnRenderImage");
            blurImage=AccessTools.DeclaredMethod(typeof(BlurOptimized),"OnRenderImage");
            if(render==null || setWidth==null || setHeight==null || setScreenWidth==null || setScreenHeight==null || outlineImage==null || blurImage==null)
                throw new MissingMethodException("Post-processing members required for display-sized post-processing are missing");
            var harmony=new Harmony("local.riskofrainupscaling.upscaling.displaypost");
            harmony.Patch(render,prefix:new HarmonyMethod(typeof(UpscalerSession),nameof(RedirectPostProcess)));
            harmony.Patch(blurImage,prefix:new HarmonyMethod(typeof(UpscalerSession),nameof(SkipRenderSizedEffect)));
            postPatched=true;
        }
        private static void RedirectPostProcess(PostProcessLayer __instance, PostProcessRenderContext context)
        {
            if(context==null || context.command!=legacyPostBuffer(__instance))return; // Only the post-transparency stack.
            if(!owners.TryGetValue(__instance,out var owner) || !owner.Upscaled || !owner.Output || !owner.PostOutput) {redirected.Remove(__instance);return;}
            try {
                int width=owner.Output.width,height=owner.Output.height;
                var size=new object[]{width};setWidth.Invoke(context,size);setScreenWidth.Invoke(context,size);
                size=new object[]{height};setHeight.Invoke(context,size);setScreenHeight.Invoke(context,size);
                ref var descriptor=ref sourceDescriptor(context);descriptor.width=width;descriptor.height=height;
                context.source=owner.Output;context.destination=owner.PostOutput;context.flip=false;
                redirected[__instance]=owner;
                if(!owner.loggedRedirect){owner.loggedRedirect=true;owner.log.Info("DISPLAY_POST_PROCESSING display-sized "+width+"x"+height+" from the DLSS output; opaque-only effects stay at "+owner.renderWidth+"x"+owner.renderHeight);}
            } catch(Exception e) {
                // A half-applied context would mix sizes; leave PPv2's render-sized stack.
                redirected.Remove(__instance);
                var failure=owner.Failure;owner.Restore();failure?.Invoke(new InvalidOperationException("Display-sized post-processing setup failed: "+e.GetBaseException().Message));
            }
        }
        // The render-sized OnRenderImage chain is discarded while the post stack is
        // redirected; its copies of these effects become plain blits.
        private static bool SkipRenderSizedEffect(MonoBehaviour __instance, RenderTexture source, RenderTexture destination)
        {
            if(displayEffects || !__instance)return true;
            var layer=__instance.GetComponent<PostProcessLayer>();
            if(!layer || !redirected.ContainsKey(layer))return true;
            Graphics.Blit(source,destination);return false;
        }
        internal bool PostRedirected => layer && redirected.TryGetValue(layer,out var owner) && owner==this;
        // Image effects that run before our tail and cannot be reproduced at display size.
        internal string UnsupportedImageEffect(MonoBehaviour tail)
        {
            const BindingFlags flags=BindingFlags.Instance|BindingFlags.Public|BindingFlags.NonPublic;
            foreach(var effect in camera.GetComponents<MonoBehaviour>()) {
                if(effect==tail)break;
                if(!effect || !effect.isActiveAndEnabled || effect is PostProcessLayer || effect is OutlineHighlight || effect is BlurOptimized)continue;
                if(effect.GetType().GetMethod("OnRenderImage",flags)!=null)return effect.GetType().FullName;
            }
            return null;
        }
        // Reruns the supported scene image effects in component order on the
        // display-sized post-processed image. Temporaries are returned to the caller.
        internal RenderTexture RunDisplayEffects(MonoBehaviour tail, List<RenderTexture> temporaries)
        {
            RenderTexture current=PostOutput;
            foreach(var effect in camera.GetComponents<MonoBehaviour>()) {
                if(effect==tail)break;
                if(!effect || !effect.isActiveAndEnabled)continue;
                var method=effect is OutlineHighlight?outlineImage:effect is BlurOptimized?blurImage:null;
                if(method==null)continue;
                var next=RenderTexture.GetTemporary(current.width,current.height,0,current.format,RenderTextureReadWrite.Linear);
                temporaries.Add(next);
                displayEffects=true;
                try {method.Invoke(effect,new object[]{current,next});}
                finally {displayEffects=false;}
                current=next;
            }
            return current;
        }
    }
}
