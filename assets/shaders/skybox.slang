import bindless;

struct PushConstants
{
    uint env_map_index;
    uint sampler_index;
    float4x4 inv_view_proj_no_translation;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 view_dir : TEXCOORD0;
};

[shader("vertex")]
VSOutput main(uint vertex_id: SV_VertexID, uniform PushConstants pc, out VSOutput output)
{
    float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    const var clip_pos = float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
    output.position = clip_pos;
    const var world_pos = mul(pc.inv_view_proj_no_translation, clip_pos);
    output.view_dir = world_pos.xyz;
    return output;
}
struct PSOut
{
    float4 color : SV_Target0;
}

[shader("fragment")]
void main(in VSOutput input, uniform PushConstants pc, out PSOut o)
{
    float3 dir = normalize(input.view_dir);
    o.color = sample_cube(pc.env_map_index, pc.sampler_index, dir);
}
