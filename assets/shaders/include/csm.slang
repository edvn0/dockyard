import push_constant;
import bindless;

static const int shadow_map_resolution = 2048;

Tuple<float, uint> compute_shadow(float3 world_pos, float3 n, float3 sun_dir,
                                  float view_depth, FrameUBO frame)
{
    uint cascade_idx = shadow_map_cascade_count - 1u;
    for (uint i = 0u; i < shadow_map_cascade_count; ++i)
    {
        if (view_depth <= frame.cascades[i].split_depth)
        {
            cascade_idx = i;
            break;
        }
    }

    float4 light_clip = mul(frame.cascades[cascade_idx].view_proj,
                            float4(world_pos, 1.0));
    float3 ndc = light_clip.xyz / light_clip.w;

    float2 uv = ndc.xy * 0.5 + 0.5;

    // Slope-scaled bias: steeper surfaces get more bias to avoid acne.
    float cos_theta = clamp(dot(n, sun_dir), 0.0, 1.0);
    float bias = max(0.005 * (1.0 - cos_theta), 0.0005);
    float shadow_z = ndc.z - bias;

    Texture2DArray<float> shadow_array = get_texture_2d_array(frame.shadow_array_index);
    SamplerComparisonState shadow_sampler = get_comparison_sampler(frame.shadow_sampler_index);

    float shadow = 0.0;
    float2 texel = 1.0 / float2(shadow_map_resolution);
    [unroll]
    for (int x = -1; x <= 1; ++x)
        [unroll]
    for (int y = -1; y <= 1; ++y)
        {
            shadow += shadow_array.SampleCmpLevelZero(
                shadow_sampler,
                float3(uv + float2(x, y) * texel, float(cascade_idx)),
                shadow_z);
        }

    return makeTuple(shadow / 9.0, cascade_idx);
}

float3 debug_cascade_color(uint cascade_idx)
{
    switch (cascade_idx)
    {
    case 0: return float3(1.0, 0.0, 0.0); // red    — nearest
    case 1: return float3(1.0, 0.5, 0.0); // orange
    case 2: return float3(1.0, 1.0, 0.0); // yellow
    case 3: return float3(0.0, 1.0, 0.0); // green
    case 4: return float3(0.0, 0.5, 1.0); // blue
    default: return float3(0.5, 0.0, 1.0); // purple — farthest
    }
}