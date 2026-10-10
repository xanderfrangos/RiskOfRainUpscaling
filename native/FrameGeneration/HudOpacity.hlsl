#include "Core/Fullscreen.hlsli"
Texture2D<float4> inputTexture : register(t0);
// HUD opacity for Frame Generation: the alpha of the HUD-only replay target.
float UiAlpha(Vertex v) : SV_Target { return saturate(inputTexture.Load(int3(v.position.xy, 0)).a); }
