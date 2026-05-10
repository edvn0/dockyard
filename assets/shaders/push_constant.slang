import handle;

struct Transform
{
    column_major float4x4 transform;
}

struct FrameUBO
{
    column_major float4x4 view;
    column_major float4x4 projection;
    column_major float4x4 view_projection;
    column_major float4x4 inverse_projection;
    column_major float4x4 inverse_view_projection;
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
};

[[vk::push_constant]]
GpuPushConstants pc;
