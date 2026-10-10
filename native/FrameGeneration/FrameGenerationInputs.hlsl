#include "Core/Fullscreen.hlsli"
Texture2D<float4> inputTexture : register(t0);
// Unity renders the scene upside down relative to the D3D swap chain, and DLAA
// keeps that layout. Frame Generation pairs depth/motion with the top-down
// presented color, so flip rows and negate the vertical motion component.
int3 FlippedPixel(int2 pixel) {
    uint width,height; inputTexture.GetDimensions(width,height);
    return int3(pixel.x,int(height)-1-pixel.y,0);
}
float FgDepthInput(Vertex v) : SV_Target { return inputTexture.Load(FlippedPixel(int2(v.position.xy))).r; }
float2 FgMotionInput(Vertex v) : SV_Target {
    float2 motion=inputTexture.Load(FlippedPixel(int2(v.position.xy))).rg;
    return float2(motion.x,-motion.y);
}
