// Upscales the render-sized interactable outline mask to the display size with
// edges as sharp as a mask drawn at the display size. The mask holds flat
// highlight colours (scaled by each highlight's fade strength) over clear.
// Bilinear filtering spreads an edge over one render pixel; scaling the
// coverage slope by the upscale factor narrows it to one display pixel.
Texture2D<float4> inputTexture : register(t0);
cbuffer Settings : register(b0) {
    uint2 outputSize;
    uint reversedDepth; // full-resolution outlines only
    uint padding;
};
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
float Weight(float4 c) { return max(max(c.r,c.g),max(c.b,c.a)); }
float4 OutlineUpscale(Vertex v) : SV_Target {
    uint width,height; inputTexture.GetDimensions(width,height);
    int2 size=int2(width,height);
    float2 position=v.position.xy*float2(size)/float2(outputSize)-.5;
    int2 base=int2(floor(position)); float2 f=position-base;
    float4 taps[4]={
        inputTexture.Load(int3(clamp(base,0,size-1),0)),
        inputTexture.Load(int3(clamp(base+int2(1,0),0,size-1),0)),
        inputTexture.Load(int3(clamp(base+int2(0,1),0,size-1),0)),
        inputTexture.Load(int3(clamp(base+int2(1,1),0,size-1),0))};
    float weights[4]={(1-f.x)*(1-f.y),f.x*(1-f.y),(1-f.x)*f.y,f.x*f.y};
    // The strongest tap supplies the colour, so a fading highlight keeps its
    // own strength and neighbouring highlights never blend into a third colour.
    float4 strongest=taps[0]; float peak=Weight(taps[0]);
    [unroll] for(int i=1;i<4;++i) if(Weight(taps[i])>peak) {peak=Weight(taps[i]);strongest=taps[i];}
    if(peak<=0) return 0;
    float coverage=0;
    [unroll] for(int j=0;j<4;++j) coverage+=weights[j]*Weight(taps[j]);
    coverage/=peak;
    float scale=max(float(outputSize.x)/width,float(outputSize.y)/height);
    return strongest*saturate((coverage-.5)*scale+.5);
}

// ---- Temporal outlines (before-post-processing DLAA/DLSS) ----
// The game's composite pass blends the highlight colour over the scene by
// (mask.r>0)*(scanline tint 0.025-0.03 + saturate(_SobelTex*41-1)). _SobelTex is
// a Sobel filter of the jittered depth, so its lines move every frame. That
// weight is carried through DLSS in the input alpha, which DLSS reconstructs
// with the scene; the tint uses the scanline average.
Texture2D<float4> secondTexture : register(t1);
Texture2D<float4> thirdTexture : register(t2);
float OutlineWeight(float4 mask, float sobel) { return mask.r>0?saturate(.0275+saturate(sobel*41-1)):0; }
// t0 render-sized HDR scene, t1 jittered highlight mask, t2 jittered Sobel:
// the sanitized DLSS input (as SanitizeHdr) with the outline weight in alpha.
float4 OutlineWeightInput(Vertex v) : SV_Target {
    int3 pixel=int3(v.position.xy,0);
    float4 c=inputTexture.Load(pixel);
    c=isfinite(c)?c:0;
    return float4(clamp(c.rgb,0,65504),OutlineWeight(secondTexture.Load(pixel),thirdTexture.Load(pixel).r));
}
// The render texel under a target pixel; the fullscreen triangle's uv is top-down like position.
int2 RenderTexel(Texture2D<float4> source, float2 uv) {
    uint width,height; source.GetDimensions(width,height);
    return clamp(int2(uv*float2(width,height)),0,int2(width,height)-1);
}
// The strongest highlight colour among the 3x3 render texels around a target
// pixel. It covers the reconstructed weight, which is unjittered.
float4 OutlineColor(Texture2D<float4> mask, float2 uv) {
    uint width,height; mask.GetDimensions(width,height);
    int2 centre=RenderTexel(mask,uv); float4 strongest=0; float peak=0;
    [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x) {
        float4 c=mask.Load(int3(clamp(centre+int2(x,y),0,int2(width,height)-1),0));
        if(c.r>0 && Weight(c)>peak) {peak=Weight(c);strongest=c;}
    }
    return strongest;
}
// t0 DLSS/DLAA result (alpha: reconstructed weight), t1 render mask: rgb is the
// highlight colour, alpha the weight. No colour nearby means no outline.
float4 OutlineResolve(Vertex v) : SV_Target {
    float4 color=OutlineColor(secondTexture,v.uv);
    return float4(color.rgb,color.r>0?saturate(inputTexture.Load(int3(v.position.xy,0)).a):0);
}
// Frames DLSS did not evaluate: the current render-sized weight, nearest texel.
float4 OutlineResolveFallback(Vertex v) : SV_Target {
    int3 texel=int3(RenderTexel(inputTexture,v.uv),0);
    float4 mask=inputTexture.Load(texel);
    return float4(mask.rgb,OutlineWeight(mask,secondTexture.Load(texel).r));
}
// t0 post-processed image, t1 resolved outline: the game's lerp, alpha kept.
float4 OutlineComposite(Vertex v) : SV_Target {
    int3 pixel=int3(v.position.xy,0);
    float4 scene=inputTexture.Load(pixel), outline=secondTexture.Load(pixel);
    return float4(lerp(scene.rgb,outline.rgb,outline.a),scene.a);
}

// ---- Full-resolution outlines (upscaling) ----
// The game's fill pass discards fragments more than 0.001 units behind
// _CameraDepthTexture at their screen UV, and its SobelBuffer pass compares
// _MainTex depths 0.002 UV apart. Drawn at the display size, both need
// display-sized depth; t0 here is the render-sized, jittered raw depth.
cbuffer FullResolution : register(b1) {
    float2 zBufferParams; // Unity's _ZBufferParams.xy: Linear01Depth = 1/(x*d+y)
    float2 sobelOffset;   // the SobelBuffer's 0.002*_ScreenParams.zw, in display pixels
};
float Nearness(float depth) { return reversedDepth?depth:1-depth; }
// The render texel position under a display pixel, in texel-centre units.
float2 RenderPosition(float2 pixel, int2 size) { return pixel*float2(size)/float2(outputSize)-.5; }
// The farthest depth among the 3x3 render texels around a display pixel. The
// unjittered fill fragment always finds its own surface there, so it never
// z-fights; an occluder hides it from one render texel inside its edge.
float OutlineOcclusionDepth(Vertex v) : SV_Target {
    uint width,height; inputTexture.GetDimensions(width,height);
    int2 size=int2(width,height), centre=int2(floor(RenderPosition(v.position.xy,size)+.5));
    float farthest=inputTexture.Load(int3(clamp(centre,0,size-1),0)).r;
    [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x) {
        float d=inputTexture.Load(int3(clamp(centre+int2(x,y),0,size-1),0)).r;
        if(Nearness(d)<Nearness(farthest)) farthest=d;
    }
    return farthest;
}
// t0 raw depth, t1 display-sized highlight mask, t2 the highlighted objects'
// own display-sized depth (far where none was drawn). Inside the mask that
// depth is exact. Where it is missing, the depth is the nearest surface among
// the 3x3 render texels (the highlighted object), and outside the mask the
// farthest, so the step sits exactly on the display-sized mask edge. Texels
// within 2% of that surface (the Sobel line threshold) are filtered
// bilinearly, so surfaces stay smooth instead of render-pixel steps.
float OutlineSobelDepth(Vertex v) : SV_Target {
    uint width,height; inputTexture.GetDimensions(width,height);
    int2 size=int2(width,height);
    float2 position=RenderPosition(v.position.xy,size);
    int2 base=int2(floor(position)), centre=int2(floor(position+.5)); float2 f=position-base;
    bool inside=Weight(secondTexture.Load(int3(v.position.xy,0)))>0;
    float object=thirdTexture.Load(int3(v.position.xy,0)).r;
    if(inside && Nearness(object)>0) return object;
    float reference=inputTexture.Load(int3(clamp(centre,0,size-1),0)).r;
    [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x) {
        float d=inputTexture.Load(int3(clamp(centre+int2(x,y),0,size-1),0)).r;
        if(inside?Nearness(d)>Nearness(reference):Nearness(d)<Nearness(reference)) reference=d;
    }
    float taps[4]={
        inputTexture.Load(int3(clamp(base,0,size-1),0)).r,
        inputTexture.Load(int3(clamp(base+int2(1,0),0,size-1),0)).r,
        inputTexture.Load(int3(clamp(base+int2(0,1),0,size-1),0)).r,
        inputTexture.Load(int3(clamp(base+int2(1,1),0,size-1),0)).r};
    float weights[4]={(1-f.x)*(1-f.y),f.x*(1-f.y),(1-f.x)*f.y,f.x*f.y};
    float sum=0,total=0;
    [unroll] for(int i=0;i<4;++i) if(abs(Nearness(taps[i])-Nearness(reference))<=.02*max(Nearness(reference),1e-6)) {sum+=weights[i]*taps[i];total+=weights[i];}
    return total>1e-4?sum/total:reference;
}
// t0 the Sobel depth above, t1 the display-sized mask. The game's SobelBuffer
// (decoded from its bytecode): eight point-sampled Linear01 depths at
// sobelOffset around the pixel, each counting only how much farther it is than
// the centre, relative to the centre, in an unweighted 3x3 Sobel. The game's
// composite turns that into saturate(sobel*41-1), a hard edge at any
// resolution, so here that weight is averaged over eight sub-pixel positions
// (the standard 8x MSAA pattern) and encoded back as (weight+1)/41, which the
// composite decodes exactly from a half-float texture.
float LinearDepth(float2 position) {
    int2 texel=clamp(int2(floor(position)),0,int2(outputSize)-1);
    return 1/(zBufferParams.x*inputTexture.Load(int3(texel,0)).r+zBufferParams.y);
}
float SobelWeight(float2 p) {
    float c=max(LinearDepth(p),.001);
    float2 o=sobelOffset;
    // Up (+y), left, right, down, then the corners.
    float a=max(LinearDepth(p+float2(0,o.y))-c,0)/c, l=max(LinearDepth(p-float2(o.x,0))-c,0)/c;
    float r=max(LinearDepth(p+float2(o.x,0))-c,0)/c, d=max(LinearDepth(p-float2(0,o.y))-c,0)/c;
    float ur=max(LinearDepth(p+o)-c,0)/c, ul=max(LinearDepth(p+float2(-o.x,o.y))-c,0)/c;
    float dr=max(LinearDepth(p+float2(o.x,-o.y))-c,0)/c, dl=max(LinearDepth(p-o)-c,0)/c;
    float gy=ur+a+ul-dr-dl-d, gx=ul+l+dl-ur-r-dr;
    return saturate(sqrt(gx*gx+gy*gy)*41-1);
}
float OutlineSobel(Vertex v) : SV_Target {
    // The composite only draws inside the mask.
    if(Weight(secondTexture.Load(int3(v.position.xy,0)))<=0) return 0;
    static const float2 samples[8]={float2(1,-3),float2(-1,3),float2(5,1),float2(-3,-5),float2(-5,5),float2(-7,-1),float2(3,7),float2(7,-7)};
    float weight=0;
    [unroll] for(int i=0;i<8;++i) weight+=SobelWeight(v.position.xy+samples[i]/16);
    weight/=8;
    return weight>0?(weight+1)/41:0;
}
