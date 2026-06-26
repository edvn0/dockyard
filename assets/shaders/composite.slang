import bindless;
import handle;
import tonemap;

struct CompositePushConstants {
  TextureHandle forward_output;
  SamplerHandle sampler;
  TextureHandle bloom_texture;
  float bloom_strength;
}

float3 tonemap_aces(float3 x) {
  const float a = 2.51f;
  const float b = 0.03f;
  const float c = 2.43f;
  const float d = 0.59f;
  const float e = 0.14f;
  return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

struct V2F {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};

[shader("vertex")]
void vertexMain(uint vertexID: SV_VertexID, out V2F output) {
  output.uv = float2((vertexID << 1) & 2, vertexID & 2);

  output.pos =
      float4(output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}

[shader("fragment")]
float4 fragmentMain(V2F input, uniform CompositePushConstants pc) : SV_Target {
  if (!pc.forward_output.isValid)
    return float4(1.0, 0.0, 1.0, 1.0);

  float3 hdr = sample_2d(pc.forward_output, pc.sampler, input.uv).rgb;

  if (pc.bloom_texture.isValid) {
    float3 bloom = sampled_images[NonUniformResourceIndex(pc.bloom_texture.index)]
        .SampleLevel(samplers[NonUniformResourceIndex(pc.sampler.index)], input.uv, 0.0).rgb;
    hdr += bloom * pc.bloom_strength;
  }

  float exposure = 1.0F;
  hdr *= exposure;

  float3 ldr = tonemap_aces(hdr);
  ldr = agx_look_punchy(ldr);

  // _SRGB swapchain target applies gamma automatically
  return float4(ldr, 1.0);
}
