using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using HarmonyLib;
using RoR2;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.PostProcessing;

namespace RiskOfRainUpscaling
{
    // Interactable outlines drawn after reconstruction (at full resolution when upscaling), and temporal outlines reconstructed by DLSS.
    internal sealed partial class UpscalerSession
    {
        // ---- Interactable outlines before post-processing ----
        // With DLSS before post-processing, OutlineHighlight draws after
        // reconstruction, so nothing anti-aliases it and the DLSS jitter moves it.
        // For pre-post owners we draw it ourselves with the game's own mask and
        // composite passes, but with the unjittered projection. Optional SMAA (the
        // game's PPv2 shader and lookup textures) takes its edges from the outline
        // alone, drawn over black, so blending touches only outline edges and the
        // already reconstructed scene stays as it was.
        private static bool outlinesPatched, outlineDrawing;
        private static MethodInfo needsHighlight, renderHighlights, clearHighlightCommands;
        private static AccessTools.FieldRef<OutlineHighlight,Material> highlightMaterial;
        private static AccessTools.FieldRef<PostProcessLayer,PostProcessResources> layerResources;
        private static PropertySheetFactory sheets;
        private static CommandBuffer smaaCommands;
        private CommandBuffer outlineCommands;
        private static readonly int edgesId=Shader.PropertyToID("_RiskOfRainUpscaling_OutlineEdges"), weightsId=Shader.PropertyToID("_RiskOfRainUpscaling_OutlineWeights");
        private string loggedOutlines;
        private static void PatchOutlines(ModLog log, bool required)
        {
            if(outlinesPatched)return;
            try {
                var image=AccessTools.DeclaredMethod(typeof(OutlineHighlight),"OnRenderImage");
                if(image==null) throw new MissingMethodException("OutlineHighlight.OnRenderImage");
                needsHighlight=AccessTools.DeclaredMethod(typeof(OutlineHighlight),"NeedsHighlight");
                renderHighlights=AccessTools.DeclaredMethod(typeof(OutlineHighlight),"RenderHighlights",new[]{typeof(RenderTexture)});
                clearHighlightCommands=AccessTools.DeclaredMethod(typeof(OutlineHighlight),"ClearCommandBuffers");
                try {
                    highlightMaterial=AccessTools.FieldRefAccess<OutlineHighlight,Material>("highlightMaterial");
                    layerResources=AccessTools.FieldRefAccess<PostProcessLayer,PostProcessResources>("m_Resources");
                    outlineDrawing=needsHighlight!=null && renderHighlights!=null && clearHighlightCommands!=null;
                } catch(Exception e) {outlineDrawing=false;log.Warning("OUTLINES members unavailable: "+e.GetBaseException().Message);}
                try {
                    uniqueHighlights=AccessTools.Field(typeof(OutlineHighlight),"uniqueHighlights");
                    preRenderHighlights=AccessTools.Field(typeof(OutlineHighlight),"onPreRenderOutlineHighlight");
                    var info=AccessTools.Inner(typeof(OutlineHighlight),"HighlightInfo");
                    infoRenderer=info!=null?AccessTools.Field(info,"renderer"):null;infoColor=info!=null?AccessTools.Field(info,"color"):null;
                    // Current builds pool the fill materials: GetMaterial(HighlightInfo),
                    // handed back by ReturnMaterials. Older ones took the colour.
                    highlightMaterialFor=info!=null?AccessTools.Method(typeof(OutlineHighlight),"GetMaterial",new[]{info}):null;
                    materialFromInfo=highlightMaterialFor!=null;
                    if(!materialFromInfo) highlightMaterialFor=AccessTools.Method(typeof(OutlineHighlight),"GetMaterial",new[]{typeof(Color)});
                    returnHighlightMaterials=AccessTools.Method(typeof(OutlineHighlight),"ReturnMaterials");
                    sobelMaterial=AccessTools.Field(typeof(RoR2.PostProcess.SobelCommandBuffer),"sobelBufferMaterial");
                    recordMembers=outlineDrawing && uniqueHighlights!=null && preRenderHighlights!=null && highlightMaterialFor!=null && infoRenderer!=null && infoColor!=null && sobelMaterial!=null &&
                        (!materialFromInfo || returnHighlightMaterials!=null);
                } catch(Exception e) {recordMembers=false;log.Warning("OUTLINES highlight recording members unavailable: "+e.GetBaseException().Message);}
                if(!recordMembers) log.Warning("OUTLINES temporal and full-resolution outlines unavailable; OutlineHighlight/SobelCommandBuffer members changed");
                new Harmony("local.riskofrainupscaling.upscaling.outlines").Patch(image,prefix:new HarmonyMethod(typeof(UpscalerSession),nameof(ReplaceOutline)));
                outlinesPatched=true;
                if(!outlineDrawing) log.Warning("OUTLINES game drawing kept (jittered, no SMAA); OutlineHighlight members changed");
            } catch(Exception e) {
                if(required) throw;
                log.Warning("OUTLINES patch failed: "+e.GetBaseException().Message+"; game outlines kept");
            }
        }
        private static bool ReplaceOutline(OutlineHighlight __instance, RenderTexture source, RenderTexture destination)
        {
            if(!__instance)return true;
            var layer=__instance.GetComponent<PostProcessLayer>();
            if(!layer)return true;
            // Display-sized post-processing: the render-sized chain is discarded.
            if(!displayEffects && redirected.ContainsKey(layer)){Graphics.Blit(source,destination);return false;}
            if(!outlineDrawing || !source || !destination || !owners.TryGetValue(layer,out var owner) || !owner.BeforePostProcessing)return true;
            try {owner.DrawOutlines(__instance,source,destination);return false;}
            catch(Exception e) {
                // The game's own path rewrites destination completely.
                outlineDrawing=false;owner.log.Warning("OUTLINES_FAILED "+e.GetBaseException().Message+"; game outline drawing restored");
                return true;
            }
        }
        // ---- Temporal outlines ----
        // The game's composite blends the highlight colour over the scene by
        // (mask>0)*(faint scanline tint + Sobel edges of the jittered depth). Those
        // edges move with the jitter every frame. Here the mask (game fill pass,
        // jittered projection, so its depth test matches the depth it reads) and the
        // Sobel edges (the game's own SobelBuffer material) are drawn before our
        // evaluation. Native code puts the weight in the DLSS input alpha, DLSS
        // reconstructs it, and the composite event blends it in at display size.
        // Recording the game's highlight draws ourselves; temporal and full-resolution outlines need them.
        private static bool recordMembers, materialFromInfo;
        private static FieldInfo uniqueHighlights, preRenderHighlights, infoRenderer, infoColor, sobelMaterial;
        private static MethodInfo highlightMaterialFor, returnHighlightMaterials;
        private CommandBuffer compositeCommands;
        private bool loggedTemporal, recordFailed;
        internal bool TemporalOutlinesActive => temporalRegistered && recordMembers && !recordFailed && Context!=IntPtr.Zero && NativeApi.UpscalerTemporalOutlinesActive(Context)==1;
        internal string OutlinePath => TemporalOutlinesActive?"temporal":FullResolutionActive?"full resolution":"after reconstruction";
        // OutlineHighlight.RenderHighlights, split so the full-resolution path can
        // draw the same renderers' depth first: Collect gathers the visible
        // highlighted renderers with their GetMaterial argument, Draw records the
        // fill pass for them. The pooled fill materials go back at once: until
        // these commands run, only our replacement of the game's pass takes
        // materials from the pool.
        private static readonly List<KeyValuePair<Renderer,object>> highlighted=new List<KeyValuePair<Renderer,object>>();
        private static bool CollectHighlights(OutlineHighlight outline)
        {
            highlighted.Clear();
            if(!outline || !outline.isActiveAndEnabled || !(bool)needsHighlight.Invoke(outline,null))return false;
            foreach(var highlight in Highlight.readonlyHighlightList) {
                if(!highlight || !highlight.isOn)continue;
                var color=highlight.GetColor()*highlight.strength;
                if(highlight.targetRenderer) outline.AddHighlight(highlight.targetRenderer,color);
                else if(highlight.GetTargetRendererList() is List<Renderer> renderers) foreach(var renderer in renderers) outline.AddHighlight(renderer,color);
            }
            (preRenderHighlights.GetValue(null) as Action<OutlineHighlight>)?.Invoke(outline);
            var unique=(System.Collections.IDictionary)uniqueHighlights.GetValue(null);
            try {
                foreach(var info in unique.Values) {
                    var renderer=(Renderer)infoRenderer.GetValue(info);
                    if(renderer && renderer.isVisible) highlighted.Add(new KeyValuePair<Renderer,object>(renderer,materialFromInfo?info:infoColor.GetValue(info)));
                }
            }
            finally {unique.Clear();}
            return highlighted.Count>0;
        }
        private static bool DrawHighlights(OutlineHighlight outline, CommandBuffer commands)
        {
            try {foreach(var highlight in highlighted) commands.DrawRenderer(highlight.Key,(Material)highlightMaterialFor.Invoke(outline,new[]{highlight.Value}),0,0);}
            finally {returnHighlightMaterials?.Invoke(outline,null);}
            return highlighted.Count>0;
        }
        private static bool RecordHighlights(OutlineHighlight outline, CommandBuffer commands) => CollectHighlights(outline) && DrawHighlights(outline,commands);
        // The game's SobelBuffer material, or null when the camera draws no Sobel edges.
        private Material SobelBuffer()
        {
            var sobel=camera.GetComponent<RoR2.PostProcess.SobelCommandBuffer>();
            return sobel && sobel.isActiveAndEnabled?sobelMaterial.GetValue(sobel) as Material:null;
        }
        // Called while rebuilding the after-transparents buffer, before the evaluation.
        internal void RecordOutlineInputs(CommandBuffer commands)
        {
            if(!TemporalOutlinesActive || !camera)return;
            try {
                commands.SetRenderTarget(outlineMask);commands.ClearRenderTarget(false,true,Color.clear);
                bool drawn=RecordHighlights(camera.GetComponent<OutlineHighlight>(),commands);
                // An empty mask gives no weight, so the edges are only needed with highlights.
                var sobelBuffer=drawn?SobelBuffer():null;
                if(drawn) {
                    if(sobelBuffer) commands.Blit(BuiltinRenderTextureType.ResolvedDepth,outlineSobel,sobelBuffer);
                    else {commands.SetRenderTarget(outlineSobel);commands.ClearRenderTarget(false,true,Color.clear);}
                }
                commands.SetRenderTarget(BuiltinRenderTextureType.CameraTarget);
                if(!loggedTemporal && drawn){loggedTemporal=true;log.Info("OUTLINES temporal: mask and Sobel edges drawn jittered at "+renderWidth+"x"+renderHeight+
                    " before "+(Upscaled?"DLSS":"DLAA")+"; weight reconstructed in the DLSS alpha and composited at "+outlineComposite.width+"x"+outlineComposite.height+"; sobel="+(sobelBuffer?"game SobelBuffer":"none (tint only)"));}
            } catch(Exception e) {
                // The after-reconstruction outline path takes over.
                recordFailed=true;commands.SetRenderTarget(BuiltinRenderTextureType.CameraTarget);
                log.Warning("OUTLINES temporal recording failed: "+e.GetBaseException().Message+"; outlines drawn after reconstruction");
            }
        }
        private void DrawOutlines(OutlineHighlight outline, RenderTexture source, RenderTexture destination)
        {
            if(!(bool)needsHighlight.Invoke(outline,null)){Graphics.Blit(source,destination);return;}
            if(TemporalOutlinesActive && outlineComposite.IsCreated() && outlineMask.IsCreated() && outlineSobel.IsCreated()) {
                // Already anti-aliased by DLSS, so no SMAA here.
                Graphics.Blit(source,outlineComposite);
                compositeCommands=compositeCommands??new CommandBuffer{name="RiskOfRainUpscaling.Upscaling.TemporalOutlines"};
                compositeCommands.Clear();compositeCommands.IssuePluginEventAndData(NativeApi.UpscalerGetOutlineCompositeEvent(),Time.frameCount,Context);
                Graphics.ExecuteCommandBuffer(compositeCommands);
                Graphics.Blit(outlineComposite,destination);
                return;
            }
            int width=destination.width,height=destination.height,scale=Math.Max(1,(int)outline.m_resolution);
            var material=highlightMaterial(outline);
            if(!material) throw new InvalidOperationException("Outline material missing");
            var temporaries=new List<RenderTexture>();
            RenderTexture Temporary(int w,int h,RenderTextureFormat format){var t=RenderTexture.GetTemporary(w,h,0,format,RenderTextureReadWrite.Linear);temporaries.Add(t);return t;}
            try {
                bool full=scale==1 && FullResolutionReady(width,height);
                RenderTexture map;string drawn;
                if(full) {map=outlineDisplayMask;drawn="full resolution (mask, depth and Sobel edges at "+width+"x"+height+"; "+DrawFullResolutionMask(outline)+")";}
                else {
                    // The fill pass occlusion-tests against _CameraDepthTexture and the
                    // composite pass steps by _ScreenParams, both render-sized. A display-sized
                    // mask point-samples the depth at a non-integer ratio and z-fights in
                    // stripes, so the mask is drawn at render size and then upscaled natively
                    // with one-display-pixel edges; if that is unavailable the composite
                    // samples it bilinearly, which is a render pixel soft.
                    int maskWidth=Upscaled?renderWidth:width,maskHeight=Upscaled?renderHeight:height;
                    bool sharpen=Upscaled && outlineSharpened && scale==1 && outlineMask.IsCreated() && outlineDisplayMask.IsCreated();
                    var mask=sharpen?outlineMask:Temporary(Math.Max(1,maskWidth/scale),Math.Max(1,maskHeight/scale),RenderTextureFormat.ARGB32);
                    var previous=RenderTexture.active;
                    RenderTexture.active=mask;GL.Clear(true,true,Color.clear);RenderTexture.active=previous;
                    clearHighlightCommands.Invoke(outline,null);
                    // Draw the highlighted renderers without the DLSS jitter, which is a
                    // render pixel (several display pixels at Ultra Performance).
                    if(jittered) {
                        unjitter=unjitter??new CommandBuffer{name="RiskOfRainUpscaling.Upscaling.UnjitteredOutlines"};
                        unjitter.Clear();unjitter.SetRenderTarget(mask);
                        unjitter.SetViewProjectionMatrices(camera.worldToCameraMatrix,original);
                        Graphics.ExecuteCommandBuffer(unjitter);
                    }
                    renderHighlights.Invoke(outline,new object[]{mask});
                    if(sharpen) {
                        outlineCommands=outlineCommands??new CommandBuffer{name="RiskOfRainUpscaling.Upscaling.OutlineUpscale"};
                        outlineCommands.Clear();outlineCommands.IssuePluginEventAndData(NativeApi.UpscalerGetOutlineUpscaleEvent(),Time.frameCount,Context);
                        Graphics.ExecuteCommandBuffer(outlineCommands);
                    }
                    map=sharpen?outlineDisplayMask:mask;
                    drawn="mask="+mask.width+"x"+mask.height+(Upscaled?" upscale="+(sharpen?"sharpened":"bilinear"):"");
                }
                material.SetTexture("_OutlineMap",map);material.SetColor("_Color",Color.white);
                var smaa=SmaaSheet();
                if(loggedOutlines!=drawn){loggedOutlines=drawn;log.Info("OUTLINES drawn by RiskOfRainUpscaling: unjittered="+jittered+" smaa="+(smaa!=null?"high (edges from outlines only)":"unavailable")+" size="+width+"x"+height+" "+drawn);}
                // Full resolution: the composite reads our display-sized Sobel edges
                // in place of the game's render-sized ones, which it sets every frame.
                var gameSobel=full?Shader.GetGlobalTexture(sobelTextureId):null;
                if(full) Shader.SetGlobalTexture(sobelTextureId,outlineDisplaySobel);
                try {
                    if(smaa==null){Graphics.Blit(source,destination,material,1);return;}
                    // The same composite over black isolates the outline for edge detection.
                    var previous=RenderTexture.active;
                    var black=Temporary(width,height,source.format);
                    RenderTexture.active=black;GL.Clear(false,true,Color.clear);RenderTexture.active=previous;
                    var lines=Temporary(width,height,source.format);
                    Graphics.Blit(black,lines,material,1);
                    var composited=Temporary(width,height,source.format);
                    Graphics.Blit(source,composited,material,1);
                    smaaCommands=smaaCommands??new CommandBuffer{name="RiskOfRainUpscaling.Upscaling.OutlineSMAA"};
                    var commands=smaaCommands;commands.Clear();
                    commands.GetTemporaryRT(edgesId,width,height,0,FilterMode.Bilinear,RenderTextureFormat.ARGB32,RenderTextureReadWrite.Linear);
                    commands.GetTemporaryRT(weightsId,width,height,0,FilterMode.Bilinear,RenderTextureFormat.ARGB32,RenderTextureReadWrite.Linear);
                    // PPv2 SMAA passes: 0-2 edge detection, 3-5 blend weights (Low/Medium/High), 6 neighborhood blending.
                    commands.BlitFullscreenTriangle(lines,edgesId,smaa,2,clear:true);
                    commands.BlitFullscreenTriangle(edgesId,weightsId,smaa,5);
                    commands.SetGlobalTexture("_BlendTex",weightsId);
                    commands.BlitFullscreenTriangle(composited,destination,smaa,6);
                    commands.ReleaseTemporaryRT(edgesId);commands.ReleaseTemporaryRT(weightsId);
                    Graphics.ExecuteCommandBuffer(commands);
                }
                finally {if(gameSobel) Shader.SetGlobalTexture(sobelTextureId,gameSobel);}
            }
            finally {foreach(var texture in temporaries)RenderTexture.ReleaseTemporary(texture);}
        }
        // ---- Full-resolution outlines ----
        // Upscaling without temporal outlines (the selected mode, or no DLSS alpha
        // upscaling with FSR) draws the mask and the Sobel edges at the display size,
        // so outline edges are as crisp as native rendering. The game's fill pass
        // discards fragments 0.001 units behind _CameraDepthTexture, so native code
        // builds a display-sized stand-in for it from the render-sized depth. The
        // highlighted objects' own depth is drawn at the display size through their
        // materials' ShadowCaster pass (how Unity builds camera depth). Native code
        // then runs the game's Sobel filter on that depth, anti-aliased over eight
        // sub-pixel positions, in place of the game's hard-edged SobelBuffer.
        private static readonly int sobelTextureId=Shader.PropertyToID("_SobelTex"), cameraDepthId=Shader.PropertyToID("_CameraDepthTexture"),
            shadowBiasId=Shader.PropertyToID("unity_LightShadowBias");
        private static readonly ShaderTagId lightMode=new ShaderTagId("LightMode");
        // Each material's ShadowCaster pass, or -1 without one.
        private static readonly Dictionary<Material,int> shadowCasterPasses=new Dictionary<Material,int>();
        private RenderTexture outlineOcclusion, outlineObjectDepth, outlineDisplaySobel;
        private CommandBuffer fullResolutionCommands;
        private int fullResolution; // 0 untried, 1 registered, -1 unavailable
        private bool FullResolutionActive => fullResolution==1 && Context!=IntPtr.Zero && NativeApi.UpscalerFullResolutionOutlinesActive(Context)==1;
        // Creates and registers the display-sized textures the first time they are needed.
        private bool FullResolutionReady(int width,int height)
        {
            if(!Upscaled || !recordMembers || State<1 || TemporalOutlinesActive || width!=Output.width || height!=Output.height || !outlineDisplayMask || !outlineDisplayMask.IsCreated())return false;
            if(fullResolution==0) {
                fullResolution=-1;
                outlineOcclusion=OutlineTexture("RiskOfRainUpscaling.Upscaling.OutlineOcclusionDepth",width,height,RenderTextureFormat.RFloat,FilterMode.Point);
                outlineDisplaySobel=OutlineTexture("RiskOfRainUpscaling.Upscaling.OutlineSobelDisplay",width,height,RenderTextureFormat.RHalf,FilterMode.Point);
                outlineObjectDepth=new RenderTexture(width,height,32,RenderTextureFormat.Depth) {
                    name="RiskOfRainUpscaling.Upscaling.OutlineObjectDepth", antiAliasing=1, useMipMap=false, autoGenerateMips=false, filterMode=FilterMode.Point
                };
                if(SystemInfo.SupportsRenderTextureFormat(RenderTextureFormat.RHalf) && SystemInfo.SupportsRenderTextureFormat(RenderTextureFormat.Depth) &&
                   outlineOcclusion.Create() && outlineDisplaySobel.Create() && outlineObjectDepth.Create() &&
                   NativeApi.UpscalerSetFullResolutionOutlines(Context,outlineDisplayMask.GetNativeTexturePtr(),outlineObjectDepth.GetNativeTexturePtr(),
                       outlineOcclusion.GetNativeTexturePtr(),outlineDisplaySobel.GetNativeTexturePtr(),SystemInfo.usesReversedZBuffer?1:0)==0)
                    fullResolution=1;
                else log.Warning("OUTLINES full-resolution outlines unavailable; outlines drawn from the render-sized mask");
            }
            return FullResolutionActive;
        }
        private static int ShadowCasterPass(Material material)
        {
            if(shadowCasterPasses.TryGetValue(material,out int pass))return pass;
            pass=material.FindPass("ShadowCaster");
            for(int i=0;pass<0 && i<material.passCount;++i) if(material.shader.FindPassTagValue(i,lightMode).name=="ShadowCaster") pass=i;
            shadowCasterPasses[material]=pass;
            return pass;
        }
        private static int SubMeshes(Renderer renderer)
        {
            Mesh mesh=null;
            if(renderer is SkinnedMeshRenderer skinned) mesh=skinned.sharedMesh;
            else if(renderer is MeshRenderer && renderer.TryGetComponent<MeshFilter>(out var filter)) mesh=filter.sharedMesh;
            return mesh?mesh.subMeshCount:0;
        }
        // Each highlighted object's depth, unjittered, through its own ShadowCaster
        // passes with the shadow bias zeroed as for camera depth. Objects without
        // one keep the render-sized depth for their Sobel edges. Returns how many drew.
        private int DrawObjectDepth(CommandBuffer commands)
        {
            int drawn=0;
            var bias=Shader.GetGlobalVector(shadowBiasId);
            commands.SetGlobalVector(shadowBiasId,Vector4.zero);
            foreach(var highlight in highlighted) {
                var renderer=highlight.Key;
                var materials=renderer.sharedMaterials;
                int count=Math.Min(materials.Length,SubMeshes(renderer));
                for(int i=0;i<count;++i) {
                    var material=materials[i];
                    int pass=material?ShadowCasterPass(material):-1;
                    if(pass<0)continue;
                    commands.DrawRenderer(renderer,material,i,pass);++drawn;
                }
            }
            commands.SetGlobalVector(shadowBiasId,bias);
            return drawn;
        }
        // Draws the object depth, then the mask with the game's fill pass testing
        // against our occlusion depth, then the Sobel edges. Returns the log detail.
        private string DrawFullResolutionMask(OutlineHighlight outline)
        {
            var commands=fullResolutionCommands=fullResolutionCommands??new CommandBuffer{name="RiskOfRainUpscaling.Upscaling.FullResolutionOutlines"};
            commands.Clear();
            var cameraDepth=Shader.GetGlobalTexture(cameraDepthId);
            bool any=CollectHighlights(outline);
            commands.IssuePluginEventAndData(NativeApi.UpscalerGetOutlineOcclusionEvent(),Time.frameCount,Context);
            commands.SetViewProjectionMatrices(camera.worldToCameraMatrix,jittered?original:camera.projectionMatrix);
            commands.SetRenderTarget(outlineDisplayMask,outlineObjectDepth);commands.ClearRenderTarget(true,true,Color.clear);
            int depthDraws=any?DrawObjectDepth(commands):0;
            // The fill pass writes no depth; a bound depth buffer would only add its depth test.
            commands.SetRenderTarget(outlineDisplayMask);commands.ClearRenderTarget(false,true,Color.clear);
            commands.SetGlobalTexture(cameraDepthId,outlineOcclusion);
            bool drawn=any && DrawHighlights(outline,commands);
            commands.SetGlobalTexture(cameraDepthId,cameraDepth?new RenderTargetIdentifier(cameraDepth):new RenderTargetIdentifier(BuiltinRenderTextureType.Depth));
            // Without the game's Sobel effect, the composite draws the tint only.
            bool sobel=drawn && SobelBuffer();
            if(sobel) commands.IssuePluginEventAndData(NativeApi.UpscalerGetOutlineSobelEvent(),Time.frameCount,Context);
            else {commands.SetRenderTarget(outlineDisplaySobel);commands.ClearRenderTarget(false,true,Color.clear);}
            Graphics.ExecuteCommandBuffer(commands);
            return "objects="+highlighted.Count+" depthDraws="+depthDraws+" sobel="+(sobel?"8x anti-aliased":"none (tint only)");
        }
        private PropertySheet SmaaSheet()
        {
            var resources=layer?layerResources(layer):null;
            var shader=resources?.shaders?.subpixelMorphologicalAntialiasing;
            if(!shader || !shader.isSupported || resources.smaaLuts==null || !resources.smaaLuts.area || !resources.smaaLuts.search)return null;
            sheets=sheets??new PropertySheetFactory();
            var sheet=sheets.Get(shader);
            sheet.properties.SetTexture("_AreaTex",resources.smaaLuts.area);
            sheet.properties.SetTexture("_SearchTex",resources.smaaLuts.search);
            return sheet;
        }
    }
}
