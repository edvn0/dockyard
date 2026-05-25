import constants;

struct PushConstants
{
    uint equirect_index; // TextureHandle index into sampled_images
    uint face_blah;      // 0-5
    uint mip;
    uint max_mip;
    uint size; // output cubemap size
    uint sampler_index;
    uint out_index;
}

float3 face_uv_to_dir(uint face, float2 uv)
{
    switch (face)
    {
    case 0: // +X
        return normalize(float3(1.0, -uv.y, -uv.x));
    case 1: // -X
        return normalize(float3(-1.0, -uv.y, uv.x));
    case 2: // +Y
        return normalize(float3(uv.x, 1.0, uv.y));
    case 3: // -Y
        return normalize(float3(uv.x, -1.0, -uv.y));
    case 4: // +Z
        return normalize(float3(uv.x, -uv.y, 1.0));
    default: // -Z
        return normalize(float3(-uv.x, -uv.y, -1.0));
    }
}

float2 dir_to_equirect_uv(float3 dir)
{
    float phi = atan2(dir.z, dir.x);             // [-π, π]
    float theta = asin(clamp(dir.y, -1.0, 1.0)); // [-π/2, π/2]
    return float2(phi / (2.0 * PI) + 0.5, theta / PI + 0.5);
}

float radical_inverse_vdc(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

float2 hammersley(uint i, uint n)
{
    return float2(float(i) / float(n), radical_inverse_vdc(i));
}

float3 importance_sample_ggx(float2 xi, float3 n, float roughness)
{
    float a = roughness * roughness;
    float phi = 2.0 * PI * xi.x;
    float cos_theta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sin_theta = sqrt(1.0 - cos_theta * cos_theta);

    float3 h = float3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);

    float3 up = abs(n.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 t = normalize(cross(up, n));
    float3 b = cross(n, t);
    return normalize(t * h.x + b * h.y + n * h.z);
}
