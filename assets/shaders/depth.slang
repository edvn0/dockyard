import push_constant;

struct V2F
{
    float4 pos : SV_Position;
};

[shader("vertex")]
V2F vertexMain(uint vertexID: SV_VulkanVertexID, uint inst_base: SV_VulkanInstanceID)
{
    V2F output;

    let remap_indices = pc.remap_buffer_ptr;
    let instance_data = pc.instance_buffer_ptr;
    let vertices = pc.position_only_buffer_ptr;

    uint global_id = remap_indices[NonUniformResourceIndex(inst_base)];
    InstanceData instance = instance_data[global_id];
    float4x4 model_matrix = instance.transform;

    float v[3] = vertices[vertexID].position;
    float4 world_pos = mul(model_matrix, float4(v[0], v[1], v[2], 1.0));
    output.pos = mul(pc.frame_ubo.view_projection, world_pos);

    return output;
}
