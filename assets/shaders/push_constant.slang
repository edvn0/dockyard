import handle;

struct InstanceData {
    float4x4 transform;
    uint material_id;
    float bounding_radius; // replaces pad0
    uint pad1;
    uint pad2;
};

struct GPUMaterial {
    float4 albedo_factor;
    float4 emissive_factor;
    float metallic_factor;

    float roughness_factor;
    float normal_scale;
    float occlusion_strength;
    uint alpha_mode;

    float alpha_cutoff;
    uint albedo_index;
    uint normal_index;
    uint metallic_roughness_index;

    uint emissive_index;
    uint occlusion_index;
    uint pad0;
};

static const int shadow_map_cascade_count = 6;
struct CascadeData {
    float4x4 view_proj;
    float split_depth; // view-space z of cascade far (negative, RH)
    float _pad[3];
};

struct FrameUBO
{
    float4x4 view;
    float4x4 projection;
    float4x4 view_projection;
    float4x4 inverse_projection;
    float4x4 inverse_view;
    float4x4 inverse_view_projection;
    float4 camera_pos;
    Array<CascadeData, shadow_map_cascade_count> cascades;
    Array<float4, 6> frustum_planes;
    uint shadow_array_index;
    uint shadow_sampler_index;
}

struct Vertex
{
    float position[3];
    uint uvs;
    uint normal;
    uint tangent;
    uint bitangent;
};

struct PositionOnlyVertex
{
    float position[3];
};

[vk::ext_scalar_block_layout]
struct VkDrawIndexedIndirectCommand {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};

struct GpuPushConstants
{
    ImmutablePtr<Vertex, AddressSpace.Device> vertex_buffer_ptr;
    ImmutablePtr<PositionOnlyVertex, AddressSpace.Device> position_only_buffer_ptr;
    ImmutablePtr<InstanceData, AddressSpace.Device> instance_buffer_ptr;
    ImmutablePtr<uint, AddressSpace.Device> culled_index_remapping_buffer;
    ImmutablePtr<FrameUBO, AddressSpace.Device> frame_ubo;
    ImmutablePtr<GPUMaterial, AddressSpace.Device> material_buffer_ptr;
    uint cascade_index;
    uint _pad[3];
};
