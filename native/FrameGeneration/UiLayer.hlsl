// FSR Frame Generation UI layer: the HUD as premultiplied color, composited by
// the FidelityFX swap chain over the HUD-less image it interpolates. Values are
// the stored (sRGB-encoded) bytes, so the real frame recomposes to the final image.
Texture2D<float4> finalColor : register(t0);
Texture2D<float4> hudlessColor : register(t1);
Texture2D<float> hudOpacity : register(t2);
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
// Measured HUD opacity: final = layer + hudless * (1 - opacity).
float4 UiLayer(Vertex v) : SV_Target
{
    int3 pixel = int3(v.position.xy, 0);
    float opacity = hudOpacity.Load(pixel);
    float3 color = finalColor.Load(pixel).rgb - hudlessColor.Load(pixel).rgb * (1 - opacity);
    return float4(max(color, 0), opacity);
}
// A HUD camera that renders into a Unity RenderTexture (it has an image effect)
// stores bottom-up rows on D3D. Its HUD-less snapshot is flipped to the swap
// chain's top-down rows, exactly (Load, no filtering).
float4 FlipRows(Vertex v) : SV_Target
{
    uint width, height; finalColor.GetDimensions(width, height);
    return finalColor.Load(int3(v.position.x, int(height) - 1 - int(v.position.y), 0));
}
// Without measured opacity, every pixel the HUD changed is opaque UI. Unchanged
// pixels are discarded onto the cleared layer, so an occlusion query counts the
// opaque coverage.
float4 UiMask(Vertex v) : SV_Target
{
    int3 pixel = int3(v.position.xy, 0);
    float3 color = finalColor.Load(pixel).rgb;
    if (all(color == hudlessColor.Load(pixel).rgb)) discard;
    return float4(color, 1);
}
