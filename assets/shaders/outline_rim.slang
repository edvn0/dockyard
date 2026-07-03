import push_constant;

// Extruded-hull selection outline: pushes each vertex outward along its
// object-space normal in clip space, scaled by 1/viewport size so the rim
// stays a constant pixel width regardless of distance or aspect ratio.
// Both the base and offset positions are put through the same full
// model-view-projection matrix, so the resulting screen-space delta is
// correct without needing a separate inverse-transpose normal matrix.
struct PC {
  float4x4 mvp;
  ImmutablePtr<Vertex, AddressSpace.Device> vertex_buffer_ptr;
  float2 inv_viewport_size;
  float outline_width_px;
  float _pad;
}

static const float4 outline_color = float4(1.0, 0.6, 0.1, 1.0);

struct V2F {
  float4 pos : SV_Position;
};

[shader("vertex")]
V2F vertexMain(uint vertex_id: SV_VulkanVertexID, uniform PC pc) {
  V2F output;

  Vertex v = pc.vertex_buffer_ptr[vertex_id];
  float3 local_pos = float3(v.position[0], v.position[1], v.position[2]);
  float3 local_normal = unpackSnorm4x8ToFloat(v.normal).xyz;

  float4 clip = mul(pc.mvp, float4(local_pos, 1.0));
  float4 clip_offset = mul(pc.mvp, float4(local_pos + local_normal, 1.0));

  float2 ndc = clip.xy / clip.w;
  float2 ndc_offset_dir = normalize(clip_offset.xy / clip_offset.w - ndc);

  ndc += ndc_offset_dir * pc.outline_width_px * 2.0 * pc.inv_viewport_size;
  clip.xy = ndc * clip.w;

  output.pos = clip;
  return output;
}

[shader("fragment")]
float4 fragmentMain(V2F input) : SV_Target {
  return outline_color;
}
