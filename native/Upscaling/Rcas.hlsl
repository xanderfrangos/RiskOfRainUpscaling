// AMD's unmodified RCAS algorithm; see third_party/FidelityFX-FSR/LICENSE.txt.
Texture2D<float4> inputTexture : register(t0);
Texture2D<float4> originalColor : register(t1);
cbuffer Settings : register(b0) {
    float sharpness;
    int2 imageSize;
    float padding;
};
#define A_GPU 1
#define A_HLSL 1
#include "../../third_party/FidelityFX-FSR/ffx_a.h"
#define FSR_RCAS_F 1
#define FSR_RCAS_DENOISE 1
#include "../../third_party/FidelityFX-FSR/ffx_fsr1.h"

AF4 FsrRcasLoadF(ASU2 p) {
    // Clamp integer loads at the display boundary, including 1-pixel dimensions.
    float4 color=inputTexture.Load(int3(clamp(p,int2(0,0),imageSize-1),0));
    // Our post-tonemap DLAA texture is linear SDR. RCAS expects display-encoded
    // values; gamma 2 fits AMD's input contract and avoids expensive pow taps.
    return float4(sqrt(saturate(color.rgb)),color.a);
}
void FsrRcasInputF(inout AF1 r,inout AF1 g,inout AF1 b) { }
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
// The original scene color is render-sized when DLSS upscales.
float OriginalAlpha(int2 pixel) {
    uint width,height,targetWidth,targetHeight;
    originalColor.GetDimensions(width,height); inputTexture.GetDimensions(targetWidth,targetHeight);
    return originalColor.Load(int3(uint2(pixel)*uint2(width,height)/uint2(targetWidth,targetHeight),0)).a;
}
float4 Rcas(Vertex v) : SV_Target {
    int2 pixel=int2(v.position.xy);
    float3 original=inputTexture.Load(int3(pixel,0)).rgb;
    if(sharpness<=0)return float4(original,OriginalAlpha(pixel));
    float3 center=FsrRcasLoadF(pixel).rgb;
    // Uniform areas need no sharpening. This also avoids the reference filter's
    // degenerate zero denominators at all-black/all-white neighborhoods.
    if(all(FsrRcasLoadF(pixel+int2(0,-1)).rgb==center) &&
       all(FsrRcasLoadF(pixel+int2(-1,0)).rgb==center) &&
       all(FsrRcasLoadF(pixel+int2(1,0)).rgb==center) &&
       all(FsrRcasLoadF(pixel+int2(0,1)).rgb==center))
        return float4(original,OriginalAlpha(pixel));
    float3 result;
    // FsrRcasCon stores exp2(-stops) in con.x. Our 0..1 strength is that
    // coefficient directly: 0.2 corresponds to about 2.32 attenuation stops.
    FsrRcasF(result.r,result.g,result.b,uint2(pixel),uint4(asuint(sharpness),0,0,0));
    result=saturate(result);
    // Restore linear SDR and preserve any original out-of-range excursions.
    result=original+(result*result-saturate(original));
    return float4(result,OriginalAlpha(pixel));
}
