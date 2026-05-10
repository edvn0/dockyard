import push_constant;

struct V2F
{
    float4 pos : SV_Position;
    float3 normal : NORMAL;
};

[shader("vertex")]
V2F vertexMain(uint vertexID: SV_VertexID, uint instanceID: SV_InstanceID)
{
    V2F output;

    let remap_indices = pc.remap_buffer_ptr;
    let transforms = pc.transform_buffer_ptr;
    let vertices = pc.vertex_buffer_ptr;

    uint global_id = remap_indices[instanceID];
    float4x4 model = transforms[global_id].transform;

    Vertex v = vertices[vertexID];
    float2 uv = unpackHalf2x16ToFloat(v.uvs);

    float3 normal = unpackSnorm4x8ToFloat(v.normal).xyz;
    float3 tangent = unpackSnorm4x8ToFloat(v.tangent).xyz;
    float3 bitangent = unpackSnorm4x8ToFloat(v.bitangent).xyz;

    output.normal = mul((float3x3)model, normal);

    float4 worldPos = mul(model, float4(v.position[0], v.position[1], v.position[2], 1.0));
    output.pos = mul(pc.frame_ubo.view_projection, worldPos);

    return output;
}

[shader("fragment")]
float4 fragmentMain(V2F input) : SV_Target
{
    return float4(input.normal * 0.5 + 0.5, 1.0);
}
