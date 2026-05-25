import push_constant;
import bindless;
import include.csm;
import include.pbr;

static const float PI = 3.14159265359f;

struct V2F
{
    float4 pos : SV_Position;
    float3 world_pos : POSITION0;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float3 bitangent : BINORMAL;
    float2 uv : TEXCOORD0;
    uint material_id : TEXCOORD1;
    float3x3 tbn : TEXCOORD2;
};

float4 sample_material_tex(uint idx, float2 uv)
{
    return sampled_images[NonUniformResourceIndex(idx)].Sample(samplers[1], uv);
}

[shader("vertex")]
V2F vertexMain(uint vertex_id: SV_VulkanVertexID, uint instance_id: SV_InstanceID, uint inst_base: SV_VulkanInstanceID, uniform GpuPushConstants pc)
{
    V2F output;

    let culled_remap_indices = pc.culled_index_remapping_buffer;
    let instance_data = pc.instance_buffer_ptr;
    let vertices = pc.vertex_buffer_ptr;

    uint global_id = culled_remap_indices[NonUniformResourceIndex(inst_base)];
    InstanceData instance = instance_data[global_id];
    float4x4 model_matrix = instance.transform;
    float3x3 normal_matrix = (float3x3)model_matrix;

    Vertex v = vertices[vertex_id];

    float3 local_normal = unpackSnorm4x8ToFloat(v.normal).xyz;
    float4 local_tangent = unpackSnorm4x8ToFloat(v.tangent).xyzw;
    float3 local_bitangent = unpackSnorm4x8ToFloat(v.bitangent).xyz;

    output.normal = normalize(mul(normal_matrix, local_normal));
    output.tangent = normalize(mul(normal_matrix, local_tangent.xyz));
    output.bitangent = normalize(mul(normal_matrix, local_bitangent));

    float4 world_pos = mul(model_matrix, float4(v.position[0], v.position[1], v.position[2], 1.0f));
    output.world_pos = world_pos.xyz;
    output.pos = mul(pc.frame_ubo.view_projection, world_pos);

    output.uv = unpackHalf2x16ToFloat(v.uvs);
    output.material_id = instance.material_id;

    float3 n = normalize(output.normal);
    float3 t = normalize(output.tangent - dot(output.tangent, n) * n);
    float3 b = cross(n, t) * local_tangent.w; // tangent_sign = ±1, stored in v.tangent.w
    output.tbn = float3x3(t, b, n);

    return output;
}

// ── Fragment ─────────────────────────────────────────────────────────────────

[shader("fragment")]
float4 fragmentMain(V2F input, uniform GpuPushConstants pc) : SV_Target
{
    GPUMaterial mat = pc.material_buffer_ptr[NonUniformResourceIndex(input.material_id)];

    float4 albedo = sample_material_tex(mat.albedo_index, input.uv) * mat.albedo_factor;
    if (mat.alpha_mode == 1u && albedo.a < mat.alpha_cutoff)
        discard;

    float3 geom_n = normalize(input.normal);
    float3 sampled_n = sample_material_tex(mat.normal_index, input.uv).xyz;
    sampled_n = sampled_n * 2.0f - 1.0f;

    sampled_n.xy *= mat.normal_scale;
    sampled_n.z = sqrt(saturate(1.0f - dot(sampled_n.xy, sampled_n.xy)));

    float3 n = normalize(mul(sampled_n, input.tbn));

    float4 mr = sample_material_tex(mat.metallic_roughness_index, input.uv);
    float roughness = clamp(mr.b * mat.roughness_factor, 0.04f, 1.0f);
    float metallic = clamp(mr.g * mat.metallic_factor, 0.0f, 1.0f);

    float3 emissive = sample_material_tex(mat.emissive_index, input.uv).rgb
                    * mat.emissive_factor.xyz * mat.emissive_factor.a;

    float occlusion = sample_material_tex(mat.occlusion_index, input.uv).r;
    float ao = 1.0f + mat.occlusion_strength * (occlusion - 1.0f);

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

    var frame = pc.frame_ubo;

    float3 irradiance = sampled_cubemaps[NonUniformResourceIndex(frame.ibl_irradiance_index)]
                            .Sample(samplers[frame.ibl_sampler_index], n) .rgb;

    float3 r = reflect(-v, n);
    float env_mip = roughness * float(frame.ibl_prefiltered_mips - 1u);
    float3 prefiltered = sampled_cubemaps[NonUniformResourceIndex(frame.ibl_prefiltered_index)]
                             .SampleLevel(samplers[frame.ibl_sampler_index], r, env_mip) .rgb;

    float2 env_brdf = sampled_images[NonUniformResourceIndex(frame.ibl_brdf_lut_index)]
                          .Sample(samplers[0], float2(ndotv_safe, roughness)) .rg;

    float3 f_ibl = fresnel_schlick_roughness(ndotv_safe, f0, roughness);
    float3 kd_ibl = (1.0f - f_ibl) * (1.0f - metallic);

    float3 diffuse_ibl = kd_ibl * albedo.rgb * irradiance;

    float3 specular_ibl = prefiltered * (f0 * env_brdf.r + env_brdf.g);

    float3 ambient = (diffuse_ibl + specular_ibl) * ao;

    float view_depth = 1.0 / input.pos.w;
    var shadow_result = compute_shadow(input.world_pos, n, sun_dir, view_depth, *pc.frame_ubo);

    float3 final_color = (direct * shadow_result._0) + ambient + emissive;

    // geom normal
    return float4(final_color, albedo.a);
}