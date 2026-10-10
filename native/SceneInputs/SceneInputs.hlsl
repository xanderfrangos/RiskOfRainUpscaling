#include "Core/Fullscreen.hlsli"
Texture2D<float4> first : register(t0);
SamplerState nearest : register(s0);
float4 RawDepth(Vertex vertex) : SV_Target
{
    float z = first.SampleLevel(nearest, vertex.uv, 0).r;
    return float4(z.xxx, 1);
}
