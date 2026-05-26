import bindless;
import handle;
import include.tonemap;

struct CompositePushConstants
{
    TextureHandle forward_output;
    SamplerHandle sampler;
}

struct V2F
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
void vertexMain(uint vertexID: SV_VertexID, out V2F output)
{
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);

    output.pos = float4(output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}

float3 tonemap_amd_gt(float3 x)
{
    // Tuning parameters
    const float P = 1.0;  // Max brightness
    const float a = 1.0;  // Contrast
    const float m = 0.22; // Linear section start
    const float l = 0.4;  // Linear section length
    const float c = 1.33; // Black tightener
    const float b = 0.0;  // Pedestal

    // Core evaluation
    float3 va = pow(max(0.0, x - b), a);
    float3 max_val = va / (pow(va, c) * (1.0 - m) + m) + l;

    // Normalize to [0, 1] range
    return min(float3(1.0, 1.0, 1.0), max_val / P);
}

float3 tonemap_aces(float3 x)
{
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

[shader("fragment")]
float4 fragmentMain(V2F input, uniform CompositePushConstants pc) : SV_Target
{
    if (!pc.forward_output.isValid)
        return float4(1.0, 0.0, 1.0, 1.0);

    float3 hdr = sample_2d(pc.forward_output, pc.sampler, input.uv).rgb;

    // Adjust your exposure as needed for AgX
    float exposure = 1;
    hdr *= exposure;

    // 1. Convert Linear HDR -> Linear LDR (Rec.709)
    float3 ldr = tonemap_aces(hdr);

    // 2. Apply look adjustments safely in linear space
    // ldr = agx_look_punchy(ldr);

    // 3. Return linear LDR. The _SRGB target automatically converts this to sRGB.
    return float4(ldr, 1.0);
}