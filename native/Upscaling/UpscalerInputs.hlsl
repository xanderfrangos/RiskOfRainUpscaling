Texture2D<float4> inputTexture : register(t0);
Texture2D<float4> originalColor : register(t1);
SamplerState linearClamp : register(s0);
#include "Core/Fullscreen.hlsli"
// The original scene color is render-sized; the target can be display-sized.
int3 OriginalPixel(int2 pixel) {
    uint width,height,targetWidth,targetHeight;
    originalColor.GetDimensions(width,height); inputTexture.GetDimensions(targetWidth,targetHeight);
    return int3(uint2(pixel)*uint2(width,height)/uint2(targetWidth,targetHeight),0);
}
float DepthInput(Vertex v) : SV_Target { return inputTexture.Load(int3(v.position.xy, 0)).r; }
float2 MotionInput(Vertex v) : SV_Target {
    // Unity: current minus previous UV. NGX: current pixel to previous pixel.
    return -inputTexture.Load(int3(v.position.xy, 0)).rg;
}
// DLSS in LDR mode expects display-encoded color. Our post-tonemap input is
// linear, so encode before evaluation and decode the result. The sRGB curve is
// extended above 1 so out-of-range excursions survive the round trip.
float3 EncodeSrgbCurve(float3 c) { c=max(c,0); return c<=0.0031308?c*12.92:1.055*pow(c,1/2.4)-0.055; }
float3 DecodeSrgbCurve(float3 c) { c=max(c,0); return c<=0.04045?c/12.92:pow((c+0.055)/1.055,2.4); }
float4 EncodeSrgb(Vertex v) : SV_Target { float4 c=inputTexture.Load(int3(v.position.xy,0)); return float4(EncodeSrgbCurve(c.rgb),c.a); }
float4 DecodeSrgb(Vertex v) : SV_Target { float4 c=inputTexture.Load(int3(v.position.xy,0)); return float4(DecodeSrgbCurve(c.rgb),c.a); }
float4 PreserveAlpha(Vertex v) : SV_Target {
    int2 pixel=int2(v.position.xy);
    return float4(inputTexture.Load(int3(pixel,0)).rgb,originalColor.Load(OriginalPixel(pixel)).a);
}
// Fallback when DLSS cannot evaluate a lower-resolution frame.
float4 Upscale(Vertex v) : SV_Target { return inputTexture.SampleLevel(linearClamp,v.uv,0); }
// Before post-processing the scene is linear HDR. Unity's NaN filter runs later
// in the post stack, so stop NaN/Inf here; one would persist in DLSS history.
float4 SanitizeHdr(Vertex v) : SV_Target {
    float4 c=inputTexture.Load(int3(v.position.xy,0));
    c=isfinite(c)?c:0;
    return float4(clamp(c.rgb,0,65504),c.a);
}
// Before-post-processing fallback when DLSS cannot evaluate a lower-resolution
// frame: the post stack reads this display-sized copy, so it must be finite too.
float4 UpscaleHdr(Vertex v) : SV_Target {
    float4 c=inputTexture.SampleLevel(linearClamp,v.uv,0);
    c=isfinite(c)?c:0;
    return float4(clamp(c.rgb,0,65504),c.a);
}
