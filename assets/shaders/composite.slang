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

float3 tonemap_reinhard(float3 color)
{
    return color / (color + float3(1.0));
}

float3 tonemap_aces(float3 color)
{
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

[shader("fragment")]
float4 fragmentMain(V2F input, uniform CompositePushConstants pc) : SV_Target
{
    if (!pc.forward_output.isValid)
        return float4(1.0, 0.0, 1.0, 1.0);

    float3 hdr = sample_2d(pc.forward_output, pc.sampler, input.uv).rgb;

    float exposure = 1;
    hdr *= exposure;

    // 1. Apply AgX
    float3 ldr = tonemap_reinhard(hdr);
    // ldr = agx_look_punchy(ldr);

    return float4(ldr, 1.0);
}
