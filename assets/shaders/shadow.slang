import push_constant;

[shader("vertex")]
float4 main(uint vertex_id: SV_VulkanVertexID,
            uint instance_id: SV_InstanceID,
            uint inst_base: SV_VulkanInstanceID,
            uniform GpuPushConstants pc) : SV_Position
{
    let culled_remap_indices = pc.culled_index_remapping_buffer;
    let instance_data = pc.instance_buffer_ptr;

    uint global_id = culled_remap_indices[NonUniformResourceIndex(inst_base)];

    var instance = unpack_instance_data(instance_data[global_id]);
    float4x4 model_matrix = instance.transform;

    float3 pos;
    if (instance.skinned_base != 0xFFFFFFFFu) {
        let pv = pc.skinned_position_buffer_ptr[instance.skinned_base + vertex_id - instance.source_vertex_offset];
        pos = float3(pv.position[0], pv.position[1], pv.position[2]);
    } else {
        let pv = pc.position_only_buffer_ptr[NonUniformResourceIndex(vertex_id)];
        pos = float3(pv.position[0], pv.position[1], pv.position[2]);
    }

    float4x4 light_vp = pc.frame_ubo->cascades[pc.cascade_index].view_proj;

    return mul(light_vp, mul(model_matrix, float4(pos, 1.0f)));
}