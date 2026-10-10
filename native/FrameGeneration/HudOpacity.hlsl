#include "Core/Fullscreen.hlsli"
Texture2D<float4> inputTexture : register(t0);
// HUD opacity for Frame Generation: the alpha of the HUD-only replay target.
float UiAlpha(Vertex v) : SV_Target { return saturate(inputTexture.Load(int3(v.position.xy, 0)).a); }
// A HUD drawn into a Unity RenderTexture has bottom-up rows on D3D; flip them to
// the swap chain's top-down rows.
float UiAlphaFlipped(Vertex v) : SV_Target {
    uint width, height; inputTexture.GetDimensions(width, height);
    return saturate(inputTexture.Load(int3(v.position.x, int(height) - 1 - int(v.position.y), 0)).a);
}
