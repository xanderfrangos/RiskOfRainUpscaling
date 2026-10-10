#include "Core/Fullscreen.hlsli"
Texture2D<float4> first : register(t0);
SamplerState nearest : register(s0);
cbuffer Parameters : register(b0) { float nearPlane, farPlane, depthRange, motionGain; uint reversedDepth; float3 padding; }
float4 DepthPreview(Vertex vertex) : SV_Target
{
    float z = first.SampleLevel(nearest, vertex.uv, 0).r;
    float denominator = reversedDepth ? nearPlane + z * (farPlane-nearPlane) : farPlane - z * (farPlane-nearPlane);
    float eye = nearPlane * farPlane / max(denominator, 1e-8);
    float value = saturate(log2(1 + eye) / log2(1 + depthRange));
    return float4(value.xxx, 1);
}
float4 MotionPreview(Vertex vertex) : SV_Target
{
    float2 motion = first.SampleLevel(nearest, vertex.uv, 0).rg;
    return float4(saturate(0.5 + motion * motionGain), saturate(length(motion) * motionGain), 1);
}
float4 FinalCopy(Vertex vertex) : SV_Target
{
    // The bound target uses top-origin D3D rows. Unity's owned RT/readback
    // path needs the opposite vertical orientation. UNORM views preserve the
    // already encoded SDR color bytes.
    return first.SampleLevel(nearest, float2(vertex.uv.x, 1 - vertex.uv.y), 0);
}
