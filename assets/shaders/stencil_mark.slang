import push_constant;

struct PC {
  float4x4 mvp;
  ImmutablePtr<Vertex, AddressSpace.Device> vertex_buffer_ptr;
}

struct V2F {
  float4 pos : SV_Position;
};

[shader("vertex")]
V2F vertexMain(uint vertex_id: SV_VulkanVertexID, uniform PC pc) {
  V2F output;
  Vertex v = pc.vertex_buffer_ptr[vertex_id];
  output.pos =
      mul(pc.mvp, float4(v.position[0], v.position[1], v.position[2], 1.0));
  return output;
}
