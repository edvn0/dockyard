import bindless;
import handle;

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

[shader("fragment")]
float4 fragmentMain(V2F input, uniform CompositePushConstants pc) : SV_Target
{
    TextureHandle tex_handle = pc.forward_output;
    SamplerHandle samp_handle = pc.sampler;

    if (tex_handle.isValid)
    {
        return sample_2d(tex_handle, samp_handle, input.uv);
    }

    // Fallback: Magenta for missing textures
    return float4(1.0, 0.0, 1.0, 1.0);
}
