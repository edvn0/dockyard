import push_constant;

struct SkinVertex {
    uint joints_0_1; // joint[0] | (joint[1] << 16)
    uint joints_2_3; // joint[2] | (joint[3] << 16)
    uint weights;    // unorm8x4, normalized to sum to 1
};

struct SkinningPushConstants {
    ImmutablePtr<Vertex,             AddressSpace.Device> src_vertices;
    ImmutablePtr<PositionOnlyVertex, AddressSpace.Device> src_positions;
    ImmutablePtr<SkinVertex,         AddressSpace.Device> skin_attrs;
    ImmutablePtr<float4x4,           AddressSpace.Device> joint_palette;
    Ptr<Vertex,             Access.ReadWrite, AddressSpace.Device> dst_vertices;
    Ptr<PositionOnlyVertex, Access.ReadWrite, AddressSpace.Device> dst_positions;
    uint src_vertex_offset;
    uint skin_vertex_offset;
    uint dst_vertex_offset;
    uint vertex_count;
    uint joint_palette_offset; // index into joint_palette (mat4 units)
    uint _pad;
};

// Packs a float4 in [-1, 1] into a uint as snorm4x8.
uint pack_snorm4x8(float4 v) {
    v = clamp(v, -1.0, 1.0);
    int4 s = int4(round(v * 127.0));
    return (uint(s.x) & 0xFFu)
         | ((uint(s.y) & 0xFFu) << 8u)
         | ((uint(s.z) & 0xFFu) << 16u)
         | ((uint(s.w) & 0xFFu) << 24u);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 tid: SV_DispatchThreadID, uniform SkinningPushConstants pc) {
    uint idx = tid.x;
    if (idx >= pc.vertex_count)
        return;

    // Unpack joint indices (u16 pairs packed into u32)
    SkinVertex skin = pc.skin_attrs[pc.skin_vertex_offset + idx];
    uint j0 =  skin.joints_0_1        & 0xFFFFu;
    uint j1 = (skin.joints_0_1 >> 16u) & 0xFFFFu;
    uint j2 =  skin.joints_2_3        & 0xFFFFu;
    uint j3 = (skin.joints_2_3 >> 16u) & 0xFFFFu;

    // Unpack weights (unorm8x4)
    float w0 = float( skin.weights        & 0xFFu) / 255.0;
    float w1 = float((skin.weights >>  8u) & 0xFFu) / 255.0;
    float w2 = float((skin.weights >> 16u) & 0xFFu) / 255.0;
    float w3 = float((skin.weights >> 24u) & 0xFFu) / 255.0;

    // Blend four joint matrices by weight
    uint base = pc.joint_palette_offset;
    float4x4 m = w0 * pc.joint_palette[base + j0]
               + w1 * pc.joint_palette[base + j1]
               + w2 * pc.joint_palette[base + j2]
               + w3 * pc.joint_palette[base + j3];

    // Rotation-only submatrix (upper-left 3x3; no translation for normals)
    float3x3 rot = float3x3(m[0].xyz, m[1].xyz, m[2].xyz);

    // Read source vertex
    Vertex src = pc.src_vertices[pc.src_vertex_offset + idx];
    float3 pos = float3(src.position[0], src.position[1], src.position[2]);

    // Transform position (full affine)
    float3 skinned_pos = mul(m, float4(pos, 1.0)).xyz;

    // Transform normal (rotation only; tangent space uses inverse-transpose but
    // glTF skinning matrices are assumed uniform-scale, so rot suffices)
    float3 norm      = unpackSnorm4x8ToFloat(src.normal).xyz;
    float3 skinned_norm = normalize(mul(rot, norm));

    // Transform tangent; preserve bitangent sign stored in W
    float4 tang4     = unpackSnorm4x8ToFloat(src.tangent);
    float3 skinned_tang = normalize(mul(rot, tang4.xyz));

    // Transform stored bitangent
    float4 btang4    = unpackSnorm4x8ToFloat(src.bitangent);
    float3 skinned_btang = normalize(mul(rot, btang4.xyz));

    // Write full skinned vertex
    Vertex dst;
    dst.position[0] = skinned_pos.x;
    dst.position[1] = skinned_pos.y;
    dst.position[2] = skinned_pos.z;
    dst.uvs       = src.uvs;
    dst.normal    = pack_snorm4x8(float4(skinned_norm,  0.0));
    dst.tangent   = pack_snorm4x8(float4(skinned_tang,  tang4.w));
    dst.bitangent = pack_snorm4x8(float4(skinned_btang, 0.0));
    pc.dst_vertices[pc.dst_vertex_offset + idx] = dst;

    // Write position-only vertex for depth/shadow passes
    PositionOnlyVertex dst_pos;
    dst_pos.position[0] = skinned_pos.x;
    dst_pos.position[1] = skinned_pos.y;
    dst_pos.position[2] = skinned_pos.z;
    pc.dst_positions[pc.dst_vertex_offset + idx] = dst_pos;
}
