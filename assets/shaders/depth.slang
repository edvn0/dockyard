import push_constant;

struct V2F
{
    float4 pos : SV_Position;
};

[shader("vertex")]
V2F vertexMain(uint vertexID: SV_VulkanVertexID, uint inst_base: SV_VulkanInstanceID, uniform GpuPushConstants pc)
{
    V2F output;

    let remap_indices = pc.culled_index_remapping_buffer;
    let instance_data = pc.instance_buffer_ptr;

    uint global_id = remap_indices[NonUniformResourceIndex(inst_base)];
    var instance = unpack_instance_data(instance_data[global_id]);
    float4x4 model_matrix = instance.transform;

    float3 pos;
    if (instance.skinned_base != 0xFFFFFFFFu) {
        let pv = pc.skinned_position_buffer_ptr[instance.skinned_base + vertexID - instance.source_vertex_offset];
        pos = float3(pv.position[0], pv.position[1], pv.position[2]);
    } else {
        let pv = pc.position_only_buffer_ptr[vertexID];
        pos = float3(pv.position[0], pv.position[1], pv.position[2]);
    }
    float4 world_pos = mul(model_matrix, float4(pos, 1.0));
    output.pos = mul(pc.frame_ubo.view_projection, world_pos);

    return output;
}
