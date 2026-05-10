struct ResourceHandle
{
    uint value;

    property uint index
    {
        get {
            return value & 0xFFFFF;
        }
    }
    property uint gen
    {
        get {
            return value >> 20;
        }
    }
    property bool isValid
    {
        get {
            return value != 0;
        }
    }
}

typedef ResourceHandle TextureHandle;
typedef ResourceHandle SamplerHandle;
