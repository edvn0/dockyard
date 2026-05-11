static const float3x3 AGX_MAT = float3x3(
    0.84242020, 0.07840919, 0.07922053,
    0.04232824, 0.87846860, 0.07916613,
    0.04237512, 0.07843144, 0.87914274);

static const float3x3 AGX_MAT_INV = float3x3(
    1.19682120, -0.10691210, -0.08981354,
    -0.05779777, 1.15141890, -0.09343411,
    -0.05783564, -0.10701230, 1.16476770);

float3 agx_default_curve(float3 x)
{
    float3 x2 = x * x;
    float3 x3 = x2 * x;

    // Standard AgX sigmoid curve approximation
    return +15.5 * x3 * x2 - 40.14 * x3 * x + 31.96 * x3 - 6.868 * x2 + 0.4298 * x + 0.0311;
}

float3 tonemap_agx(float3 color)
{
    const float min_ev = -12.4f;
    const float max_ev = 4.0f;

    // 1. Transform to AgX Space
    color = mul(AGX_MAT, color);

    // 2. Log encoding
    color = clamp(color, 1e-10, 10000.0); // Avoid log(0)
    color = log2(color);
    color = (color - min_ev) / (max_ev - min_ev);
    color = clamp(color, 0.0, 1.0);

    // 3. Apply the AgX sigmoid curve
    color = agx_default_curve(color);

    // 4. Transform back to Linear Rec.709
    color = mul(AGX_MAT_INV, color);

    return color;
}

float3 agx_look_punchy(float3 color)
{
    const float3 luma_weights = float3(0.2126, 0.7152, 0.0722);
    float luma = dot(color, luma_weights);

    // Slight saturation boost and contrast tweak
    float3 punchy = pow(color, 1.2);
    color = lerp(luma.xxx, punchy, 1.1);

    return color;
}
