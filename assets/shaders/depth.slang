import push_constant;
import bindless;

struct V2F
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    uint material_id : TEXCOORD1;
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
    float2 uv = float2(0.0f, 0.0f);
    if (instance.skinned_base != 0xFFFFFFFFu) {
        let sv = pc.skinned_vertex_buffer_ptr[instance.skinned_base + vertexID - instance.source_vertex_offset];
        pos = float3(sv.position[0], sv.position[1], sv.position[2]);
        uv = unpackHalf2x16ToFloat(sv.uvs);
    } else {
        let pv = pc.vertex_buffer_ptr[vertexID];
        pos = float3(pv.position[0], pv.position[1], pv.position[2]);
        uv = unpackHalf2x16ToFloat(pv.uvs);
    }
    float4 world_pos = mul(model_matrix, float4(pos, 1.0));
    output.pos = mul(pc.frame_ubo.view_projection, world_pos);
    output.uv = uv;
    output.material_id = instance.material_id;

    return output;
}

[shader("fragment")]
void fragmentMain(V2F input, uniform GpuPushConstants pc)
{
    GPUMaterial mat = pc.material_buffer_ptr[NonUniformResourceIndex(input.material_id)];
    if ((mat.flags & MaterialFlags::alpha_mask) != MaterialFlags::none) {
        float alpha = sampled_images[NonUniformResourceIndex(mat.albedo_index)].Sample(samplers[1], input.uv).a;
        if (alpha < mat.alpha_cutoff)
            discard;
    }
}
