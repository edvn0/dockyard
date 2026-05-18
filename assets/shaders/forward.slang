import push_constant;
import bindless;
import include.csm;

static const float PI = 3.14159265359f;

struct V2F
{
    float4 pos : SV_Position;
    float3 world_pos : POSITION0; // <-- new
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float3 bitangent : BINORMAL;
    float2 uv : TEXCOORD0;
    uint material_id : TEXCOORD1;
    float3x3 tbn : TEXCOORD2;
};

// ── PBR helpers ─────────────────────────────────────────────────────────────

float3 fresnel_schlick(float cos_theta, float3 f0)
{
    return f0 + (1.0f - f0) * pow(clamp(1.0f - cos_theta, 0.0f, 1.0f), 5.0f);
}

float distribution_ggx(float3 n, float3 h, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float ndoth = max(dot(n, h), 0.0f);
    float denom = ndoth * ndoth * (a2 - 1.0f) + 1.0f;
    return a2 / (PI * denom * denom);
}

float geometry_schlick_ggx(float ndotx, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return ndotx / (ndotx * (1.0f - k) + k);
}

float geometry_smith(float3 n, float3 v, float3 l, float roughness)
{
    float ndotv = max(dot(n, v), 0.0f);
    float ndotl = max(dot(n, l), 0.0f);
    return geometry_schlick_ggx(ndotv, roughness)
         * geometry_schlick_ggx(ndotl, roughness);
}

float4 sample_material_tex(uint idx, float2 uv)
{
    return sampled_images[NonUniformResourceIndex(idx)].Sample(samplers[1], uv);
}

[shader("vertex")]
V2F vertexMain(uint vertex_id: SV_VulkanVertexID, uint instance_id: SV_InstanceID, uint inst_base: SV_VulkanInstanceID)
{
    V2F output;

    let remap_indices = pc.remap_buffer_ptr;
    let instance_data = pc.instance_buffer_ptr;
    let vertices = pc.vertex_buffer_ptr;

    uint global_id = remap_indices[NonUniformResourceIndex(inst_base)];
    InstanceData instance = instance_data[global_id];
    float4x4 model_matrix = instance.transform;
    float3x3 normal_matrix = (float3x3)model_matrix;

    Vertex v = vertices[vertex_id];

    float3 local_normal = unpackSnorm4x8ToFloat(v.normal).xyz;
    float3 local_tangent = unpackSnorm4x8ToFloat(v.tangent).xyz;
    float3 local_bitangent = unpackSnorm4x8ToFloat(v.bitangent).xyz;

    output.normal = normalize(mul(normal_matrix, local_normal));
    output.tangent = normalize(mul(normal_matrix, local_tangent));
    output.bitangent = normalize(mul(normal_matrix, local_bitangent));

    float4 world_pos = mul(model_matrix, float4(v.position[0], v.position[1], v.position[2], 1.0f));
    output.world_pos = world_pos.xyz; // <-- new
    output.pos = mul(pc.frame_ubo.view_projection, world_pos);

    output.uv = unpackHalf2x16ToFloat(v.uvs);
    output.material_id = instance.material_id;

    float3 n = normalize(output.normal);
    float3 t = normalize(output.tangent - dot(output.tangent, n) * n);
    float3 b = normalize(output.bitangent);
    output.tbn = float3x3(t, b, n);

    return output;
}

// ── Fragment ─────────────────────────────────────────────────────────────────

[shader("fragment")]
float4 fragmentMain(V2F input) : SV_Target
{
    GPUMaterial mat = pc.material_buffer_ptr[NonUniformResourceIndex(input.material_id)];

    // Albedo + alpha test
    float4 albedo = sample_material_tex(mat.albedo_index, input.uv) * mat.albedo_factor;
    if (mat.alpha_mode == 1u && albedo.a < mat.alpha_cutoff)
        discard;

    // Normal map
    float3 sampled_n = sample_material_tex(mat.normal_index, input.uv).xyz;
    sampled_n = sampled_n * 2.0f - 1.0f;
    sampled_n.xy *= mat.normal_scale;
    float3 n = normalize(mul(normalize(sampled_n), input.tbn));

    // Metallic-roughness (glTF: G = roughness, B = metallic)
    float4 mr = sample_material_tex(mat.metallic_roughness_index, input.uv);
    float roughness = clamp(mr.g * mat.roughness_factor, 0.04f, 1.0f);
    float metallic = clamp(mr.b * mat.metallic_factor, 0.0f, 1.0f);

    // Emissive
    float3 emissive = sample_material_tex(mat.emissive_index, input.uv).rgb
                    * mat.emissive_factor.xyz * mat.emissive_factor.a;

    // Occlusion
    float occlusion = sample_material_tex(mat.occlusion_index, input.uv).r;
    float ao = 1.0f + mat.occlusion_strength * (occlusion - 1.0f);

    // Vectors
    float3 v = normalize(pc.frame_ubo.camera_pos.xyz - input.world_pos);

    // Dielectric base reflectance 0.04; metals use albedo as f0
    float3 f0 = lerp(float3(0.04f), albedo.rgb, metallic);

    // ── Directional sun light ────────────────────────────────────────────────
    const float3 sun_dir = normalize(float3(0.5f, 1.0f, -0.3f));
    const float3 sun_color = float3(1.0f, 0.95f, 0.8f);

    float3 h = normalize(v + sun_dir);
    float ndotl = max(dot(n, sun_dir), 0.0f);
    float ndotv = max(dot(n, v), 0.0f);

    float3 f = fresnel_schlick(max(dot(h, v), 0.0f), f0);
    float ndf = distribution_ggx(n, h, roughness);
    float g = geometry_smith(n, v, sun_dir, roughness);

    float3 specular = (ndf * g * f) / max(4.0f * ndotv * ndotl, 1e-4f);

    // Energy conservation: metals have no diffuse
    float3 kd = (1.0f - f) * (1.0f - metallic);
    float3 diffuse = kd * albedo.rgb / PI;

    float3 direct = (diffuse + specular) * sun_color * ndotl;

    // ── Ambient (simple split-sum approximation) ─────────────────────────────
    const float3 ambient_color = float3(0.1f, 0.1f, 0.15f);
    float3 f_ambient = fresnel_schlick(ndotv, f0);
    float3 kd_ambient = (1.0f - f_ambient) * (1.0f - metallic);
    float3 ambient = (kd_ambient * albedo.rgb + f_ambient * f0) * ambient_color;

    float view_depth = 1.0 / input.pos.w;
    var shadow_result = compute_shadow(input.world_pos, n, sun_dir, view_depth, *pc.frame_ubo);

    float3 final_color = shadow_result._0 * (direct + ambient) * ao + emissive;

    return float4(final_color, albedo.a);
}