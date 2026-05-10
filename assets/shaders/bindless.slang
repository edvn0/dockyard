import handle;

[[vk::binding(0, 0)]]
Texture2D<float4> sampled_images[];
[[vk::binding(1, 0)]]
SamplerState samplers[];
[[vk::binding(2, 0)]]
RWTexture2D<float4> storage_images[];
[[vk::binding(3, 0)]]
SamplerComparisonState comparison_samplers[];
[[vk::binding(4, 0)]]
TextureCube<float4> sampled_cubemaps[];
[[vk::binding(5, 0)]]
Texture3D<float4> sampled_3d_images[];

float4 sample_2d(TextureHandle tex, SamplerHandle samp, float2 uv)
{
    if (!tex.isValid)
        return 0.0.xxxx;
    return sampled_images[NonUniformResourceIndex(tex.index)]
        .Sample(samplers[NonUniformResourceIndex(samp.index)], uv);
}

float4 sample_2d_lod(TextureHandle tex, SamplerHandle samp, float2 uv, float lod)
{
    if (!tex.isValid)
        return 0.0.xxxx;
    return sampled_images[NonUniformResourceIndex(tex.index)]
        .SampleLevel(samplers[NonUniformResourceIndex(samp.index)], uv, lod);
}

float4 sample_cube(TextureHandle tex, SamplerHandle samp, float3 dir)
{
    return sampled_cubemaps[NonUniformResourceIndex(tex.index)]
        .Sample(samplers[NonUniformResourceIndex(samp.index)], dir);
}

float4 sample_3d(TextureHandle tex, SamplerHandle samp, float3 uvw)
{
    return sampled_3d_images[NonUniformResourceIndex(tex.index)]
        .Sample(samplers[NonUniformResourceIndex(samp.index)], uvw);
}

float sample_shadow(TextureHandle tex, SamplerHandle samp, float2 uv, float compare_val)
{
    return sampled_images[NonUniformResourceIndex(tex.index)]
        .SampleCmpLevelZero(comparison_samplers[NonUniformResourceIndex(samp.index)], uv, compare_val);
}

float4 load_storage(TextureHandle tex, uint2 coords)
{
    return storage_images[NonUniformResourceIndex(tex.index)][coords];
}

void store_storage(TextureHandle tex, uint2 coords, float4 value)
{
    storage_images[NonUniformResourceIndex(tex.index)][coords] = value;
}

// Specialization for RW types
RWTexture2D<float4> get_rw_tex(TextureHandle h)
{
    return storage_images[NonUniformResourceIndex(h.index)];
}
