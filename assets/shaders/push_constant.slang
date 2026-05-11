import handle;

struct Transform
{
    float4x4 transform;
}

struct GPUMaterial {
    float4 albedo_factor;
    float3 emissive_factor;
    float metallic_factor; // This will sit perfectly in the 4th slot of the emissive 'row'

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
    uint pad1;
};
struct FrameUBO
{
    float4x4 view;
    float4x4 projection;
    float4x4 view_projection;
    float4x4 inverse_projection;
    float4x4 inverse_view_projection;
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

struct GpuPushConstants
{
    ImmutablePtr<Vertex> vertex_buffer_ptr;
    ImmutablePtr<PositionOnlyVertex> position_only_buffer_ptr;
    ImmutablePtr<Transform> transform_buffer_ptr;
    ImmutablePtr<uint> remap_buffer_ptr;
    ImmutablePtr<FrameUBO> frame_ubo;
    ImmutablePtr<GPUMaterial> material_buffer_ptr;
};

[[vk::push_constant]]
GpuPushConstants pc;
