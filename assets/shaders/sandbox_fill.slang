// Proof-of-concept compute shader: writes (tid.x + 1) into a BDA-addressed buffer.
struct FillPC
{
    Ptr<uint, Access.ReadWrite, AddressSpace.Device> output;
    uint element_count;
    uint _pad;
};

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 tid: SV_DispatchThreadID, uniform FillPC pc)
{
    if (tid.x >= pc.element_count)
        return;
    pc.output[tid.x] = tid.x + 1u;
}
