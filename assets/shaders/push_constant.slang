import handle;

struct InstanceData
{
    float4x3 transform;
    float material_and_lod;
    float bounding_radius;
    float padding0;
    float padding1;
};

enum class MaterialFlags {
    None = 0,
    depth_prepass = 1 << 0,    // Render in depth prepass (opaque/masked)
    alpha_mask = 1 << 1,       // Alpha testing enabled
    has_transmission = 1 << 2, // Shader: check before sampling transmission
    has_anisotropy = 1 << 3,   // Shader: check before using anisotropy
    two_sided = 1 << 4,        // Backface rendering enabled
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

    MaterialFlags flags;

    float transmission_factor; // [0,1]: 0 = opaque, 1 = fully transmissive

    float anisotropy_factor;   // [0,1]: strength of anisotropic reflection
    float anisotropy_rotation; // [0,1]: rotation angle (normalized to [0, 2π])

    uint cull_mode;

    float uv_scale_x;
    float uv_scale_y;
    float uv_offset_x;
    float uv_offset_y;
};

static const int shadow_map_cascade_count = 6;
struct CascadeData {
    float4x4 view_proj;
    float split_depth; // view-space z of cascade far (negative, RH)
    float _pad[3];
};
// 64 + 16 = 80 bytes

struct FrameUBO
{
    float4x4 view;                                         // 64 bytes
    float4x4 projection;                                   // 64 bytes
    float4x4 view_projection;                              // 64 bytes
    float4x4 inverse_projection;                           // 64 bytes
    float4x4 inverse_view;                                 // 64 bytes
    float4x4 inverse_view_projection;                      // 64 bytes
    Array<CascadeData, shadow_map_cascade_count> cascades; // 80 * 6 = 480 bytes
    Array<float4, 6> frustum_planes;                       // 16 * 4 = 96 bytes
    float4 camera_pos;                                     // 16 bytes
    float4 sun_direction;                                  // 16 bytes
    uint shadow_array_index;                               // 4 bytes
    uint shadow_sampler_index;                             // 4 bytes
    uint ibl_irradiance_index;                             // binding 4 (sampled_cubemaps)
    uint ibl_prefiltered_index;                            // binding 4 (sampled_cubemaps)
    uint ibl_brdf_lut_index;                               // binding 0 (sampled_images)
    uint ibl_sampler_index;                                // binding 1 (samplers) — linear + mip
    uint ibl_prefiltered_mips;                             // needed for roughness LOD selection
    uint _pad[3];                                          // pad to 16-byte multiple                                        // padding to make size a multiple of 16 bytes
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

struct UnpackedInstanceData
{
    float4x4 transform;
    uint material_id;
    float bounding_radius;
    uint lod_count;
};
UnpackedInstanceData unpack_instance_data(InstanceData compressed)
{
    UnpackedInstanceData unpacked;
    unpacked.transform = float4x4(
        compressed.transform[0].x, compressed.transform[1].x, compressed.transform[2].x, compressed.transform[3].x,
        compressed.transform[0].y, compressed.transform[1].y, compressed.transform[2].y, compressed.transform[3].y,
        compressed.transform[0].z, compressed.transform[1].z, compressed.transform[2].z, compressed.transform[3].z,
        0.0, 0.0, 0.0, 1.0
    );
    uint meta_bits = asuint(compressed.material_and_lod);
    unpacked.material_id = meta_bits & 0xFFFFu;
    unpacked.lod_count = (meta_bits >> 16u) & 0x7u;
    unpacked.bounding_radius = compressed.bounding_radius;

    return unpacked;
}