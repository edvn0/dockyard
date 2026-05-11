import push_constant;

struct V2F
{
    float4 pos : SV_Position;
    float3 normal : NORMAL;
};

[shader("vertex")]
V2F vertexMain(uint vertex_id: SV_VertexID, uint instance_id: SV_InstanceID)
{
    V2F output;

    let remap_indices = pc.remap_buffer_ptr;
    let transforms = pc.transform_buffer_ptr;
    let vertices = pc.vertex_buffer_ptr;

    uint global_id = remap_indices[instance_id];
    float4x4 model_matrix = transforms[global_id].transform;

    Vertex v = vertices[vertex_id];

    // Transform normal to world space
    float3 local_normal = unpackSnorm4x8ToFloat(v.normal).xyz;
    output.normal = normalize(mul((float3x3)model_matrix, local_normal));

    float4 world_pos = mul(model_matrix, float4(v.position[0], v.position[1], v.position[2], 1.0));
    output.pos = mul(pc.frame_ubo.view_projection, world_pos);

    return output;
}

[shader("fragment")]
float4 fragmentMain(V2F input) : SV_Target
{
    // Lighting parameters
    const float3 sun_direction = normalize(float3(0.5, 1.0, -0.3));
    const float3 sun_color = float3(1.0, 0.95, 0.8);
    const float3 ambient_color = float3(0.1, 0.1, 0.15);

    // Re-normalize interpolated normal
    float3 n = normalize(input.normal);
    float3 l = sun_direction;

    // Diffuse calculation (Lambert)
    float diffuse_intensity = max(dot(n, l), 0.0);
    float3 diffuse_light = diffuse_intensity * sun_color;

    // Combine results
    float3 final_color = diffuse_light + ambient_color;

    return float4(final_color, 1.0);
}