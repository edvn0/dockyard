struct LineVertex
{
    float4 pos;
    float4 color;
};

struct PC
{
    float4x4 mvp;
    ImmutablePtr<LineVertex> line_vertices;
}

struct VertexOutput
{
    float4 pos : SV_Position;
    float4 color : TEXCOORD0;
};
[shader("vertex")]
void vs_main(uint vertex_id: SV_VulkanVertexID, uniform PC pc, out VertexOutput out)
{
    LineVertex vertex = pc.line_vertices[vertex_id];
    printf("%d", vertex_id);
    out.pos = mul(pc.mvp, vertex.pos);
    out.color = vertex.color;
}

[shader("fragment")]
void fs_main(VertexOutput in, out float4 color: SV_Target0)
{
    color = in.color;
}
