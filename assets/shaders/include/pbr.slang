import constants;

float3 fresnel_schlick(float cos_theta, float3 f0)
{
    return f0 + (1.0f - f0) * pow(clamp(1.0f - cos_theta, 0.0f, 1.0f), 5.0f);
}

float3 fresnel_schlick_roughness(float cos_theta, float3 f0, float roughness)
{
    float3 r = max(float3(1.0f - roughness), f0);
    return f0 + (r - f0) * pow(clamp(1.0f - cos_theta, 0.0f, 1.0f), 5.0f);
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
    return geometry_schlick_ggx(ndotv, roughness) * geometry_schlick_ggx(ndotl, roughness);
}
