import push_constant;

struct ResetPushConstants {
  Ptr<uint, Access.ReadWrite, AddressSpace.Device> base;
  uint element_count;
  uint stride_in_u32;
  uint offset_in_u32;
  uint reset_value;
};

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 dtid: SV_DispatchThreadID,
                 uniform ResetPushConstants pc) {
  uint i = dtid.x;
  if (i >= pc.element_count)
    return;
  pc.base[i * pc.stride_in_u32 + pc.offset_in_u32] = pc.reset_value;
}
