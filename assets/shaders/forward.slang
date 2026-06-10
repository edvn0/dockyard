import push_constant;
import bindless;
import include.csm;
import include.pbr;

static const float PI = 3.14159265359f;

// OPTIMIZATION: Stripped down interpolators to bare minimum to save parameter
// cache space.
struct V2F {
    float4 pos : SV_Position;
    float3 world_pos : POSITION0;
    float3 normal : NORMAL;
    float4 tangent : TANGENT; // XYZ = Tangent, W = Bitangent Sign
    float2 uv : TEXCOORD0;
    uint material_id : TEXCOORD1;
    uint lod_idx : TEXCOORD2;
};

float4 sample_material_tex(uint idx, float2 uv) {
    return sampled_images[NonUniformResourceIndex(idx)].Sample(samplers[1], uv);
}

[shader("vertex")]
V2F vertexMain(uint vertex_id: SV_VulkanVertexID,
               uint instance_id: SV_InstanceID,
               uint inst_base: SV_VulkanInstanceID,
               uniform GpuPushConstants pc) {
    V2F output;

    let culled_remap_indices = pc.culled_index_remapping_buffer;
    let instance_data = pc.instance_buffer_ptr;
    let vertices = pc.vertex_buffer_ptr;

    uint global_id = culled_remap_indices[NonUniformResourceIndex(inst_base)];
    var instance = unpack_instance_data(instance_data[global_id]);
    float4x4 model_matrix = instance.transform;
    float3x3 normal_matrix = (float3x3)model_matrix;

    Vertex v = vertices[vertex_id];

    uint packed_data = culled_remap_indices[inst_base];
    uint true_instance_id = packed_data & 0x0FFFFFFF;
    output.lod_idx = packed_data >> 28u;

    float3 local_normal = unpackSnorm4x8ToFloat(v.normal).xyz;
    float4 local_tangent = unpackSnorm4x8ToFloat(v.tangent).xyzw;

    output.normal = normalize(mul(normal_matrix, local_normal));
    // Keep tangent sign in W, pass directly to fragment
    output.tangent =
        float4(normalize(mul(normal_matrix, local_tangent.xyz)), -local_tangent.w);

    float4 world_pos = mul(
        model_matrix, float4(v.position[0], v.position[1], v.position[2], 1.0f));
    output.world_pos = world_pos.xyz;
    output.pos = mul(pc.frame_ubo.view_projection, world_pos);

    output.uv = unpackHalf2x16ToFloat(v.uvs);
    output.material_id = instance.material_id;

    return output;
}

[shader("fragment")]
float4 fragmentMain(V2F input, uniform GpuPushConstants pc) : SV_Target {
    GPUMaterial mat =
        pc.material_buffer_ptr[NonUniformResourceIndex(input.material_id)];

    float4 albedo =
        sample_material_tex(mat.albedo_index, input.uv) * mat.albedo_factor;

    [branch]
    if (mat.alpha_mode == 1u && albedo.a < mat.alpha_cutoff)
        discard;

    float3 geom_n = normalize(input.normal);
    float3 t =
        normalize(input.tangent.xyz - dot(input.tangent.xyz, geom_n) * geom_n);
    float3 b = cross(geom_n, t) * input.tangent.w;
    float3x3 tbn = float3x3(t, b, geom_n);

    float4 normal_sample = sample_material_tex(mat.normal_index, input.uv);
    float2 normal_xy = normal_sample.rg * 2.0f - 1.0f;
    normal_xy *= mat.normal_scale;

    float3 sampled_n;
    sampled_n.xy = normal_xy;
    sampled_n.z = sqrt(1.0f - dot(sampled_n.xy, sampled_n.xy));
    float3 n = normalize(mul(sampled_n, tbn));

    float4 mr = sample_material_tex(mat.metallic_roughness_index, input.uv);
    float roughness = clamp(mr.g * mat.roughness_factor, 0.04f, 1.0f);
    float metallic = clamp(mr.b * mat.metallic_factor, 0.0f, 1.0f);

    float3 emissive = sample_material_tex(mat.emissive_index, input.uv).rgb *
                      mat.emissive_factor.xyz * mat.emissive_factor.a;

    float ao = 1.0f;
    if ((mat.flags & MaterialFlags::combined_orm) != MaterialFlags::none) {
        ao = saturate(1.0f + mat.occlusion_strength * (mr.r - 1.0f));
    } else if ((mat.flags & MaterialFlags::no_occlusion) == MaterialFlags::none) {
        float occlusion = sample_material_tex(mat.occlusion_index, input.uv).r;
        ao = saturate(1.0f + mat.occlusion_strength * (occlusion - 1.0f));
    }

    float3 v = normalize(pc.frame_ubo.camera_pos.xyz - input.world_pos);
    float3 f0 = lerp(float3(0.04f), albedo.rgb, metallic);
    float ndotv_safe = max(dot(n, v), 1e-5f);

    const float3 sun_color = float3(1.0f, 0.95f, 0.8f);
    const float3 sun_dir = normalize(pc.frame_ubo.sun_direction.xyz);

    float3 h = normalize(v + sun_dir);
    float ndotl = max(dot(n, sun_dir), 0.0f);

    float horizon = saturate(1.0f + dot(geom_n, sun_dir));
    ndotl *= horizon * horizon;

    float3 f = fresnel_schlick(max(dot(h, v), 0.0f), f0);
    float ndf = distribution_ggx(n, h, roughness);
    float g = geometry_smith(n, v, sun_dir, roughness);

    float3 specular = (ndf * g * f) / max(4.0f * ndotv_safe * ndotl, 1e-4f);
    float3 kd = (1.0f - f) * (1.0f - metallic);
    float3 diffuse = kd * albedo.rgb / PI;

    float3 direct = (diffuse + specular) * sun_color * ndotl;

    float3 irradiance = sampled_cubemaps[NonUniformResourceIndex(
                                             pc.frame_ubo.ibl_irradiance_index)]
                            .Sample(samplers[pc.frame_ubo.ibl_sampler_index], n)
                            .rgb;

    float3 r = reflect(-v, n);
    float env_mip = roughness * float(pc.frame_ubo.ibl_prefiltered_mips - 1u);
    float3 prefiltered =
        sampled_cubemaps[NonUniformResourceIndex(
                             pc.frame_ubo.ibl_prefiltered_index)]
            .SampleLevel(samplers[pc.frame_ubo.ibl_sampler_index], r, env_mip)
            .rgb;

    float2 env_brdf =
        sampled_images[NonUniformResourceIndex(pc.frame_ubo.ibl_brdf_lut_index)]
            .Sample(samplers[0], float2(ndotv_safe, roughness))
            .rg;

    float3 f_ibl = fresnel_schlick_roughness(ndotv_safe, f0, roughness);
    float3 kd_ibl = (1.0f - f_ibl) * (1.0f - metallic);

    float3 diffuse_ibl = kd_ibl * albedo.rgb * irradiance;
    float3 specular_ibl = prefiltered * (f0 * env_brdf.r + env_brdf.g);
    float3 ambient = (diffuse_ibl + specular_ibl) * ao;

    var shadow_result =
        compute_shadow(input.world_pos, geom_n, sun_dir, *pc.frame_ubo);

    float3 point_light_contribution = float3(0.0f);

    for (uint i = 0u; i < pc.frame_ubo.point_light_count; i++) {
        PointLight light = pc.frame_ubo.point_lights[i];

        float3 l = light.position - input.world_pos;
        float dist = length(l);
        if (dist >= light.radius)
            continue;

        l = l / dist;

        float window = 1.0f - pow(saturate(dist / light.radius), 4.0f);
        float attenuation = (window * window) / max(dist * dist, 1e-4f);
        float3 radiance = light.color * light.intensity * attenuation;

        float3 h_l = normalize(v + l);
        float ndotl_l = max(dot(n, l), 0.0f);

        float3 f_l = fresnel_schlick(max(dot(h_l, v), 0.0f), f0);
        float ndf_l = distribution_ggx(n, h_l, roughness);
        float g_l = geometry_smith(n, v, l, roughness);

        float3 spec_l =
            (ndf_l * g_l * f_l) / max(4.0f * ndotv_safe * ndotl_l, 1e-4f);
        float3 kd_l = (1.0f - f_l) * (1.0f - metallic);

        point_light_contribution +=
            (kd_l * albedo.rgb / PI + spec_l) * radiance * ndotl_l;
    }

    float3 final_color = (direct * shadow_result._0) + ambient + emissive +
                         point_light_contribution;
    return float4(final_color, albedo.a);
}
