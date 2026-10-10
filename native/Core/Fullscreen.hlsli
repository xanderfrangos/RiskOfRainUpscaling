struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex Fullscreen(uint id : SV_VertexID)
{
    Vertex vertex;
    vertex.uv = float2((id << 1) & 2, id & 2);
    vertex.position = float4(vertex.uv * float2(2,-2) + float2(-1,1), 0, 1);
    return vertex;
}
