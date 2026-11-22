ByteAddressBuffer gBgra;
RWByteAddressBuffer gPlanes;

cbuffer FrameParams
{
    uint gWidth;
    uint gHeight;
    uint gBgraPitch;
    uint gPlanePitch;
}

uint packChannel(uint4 pixels, uint shift)
{
    uint4 bytes = (pixels >> shift) & 0xffu;
    return bytes.x | (bytes.y << 8) | (bytes.z << 16) | (bytes.w << 24);
}

[numthreads(16, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    uint x = thread.x * 4;
    uint y = thread.y;
    if (x >= gWidth || y >= gHeight)
        return;

    // Copy the stored sRGB bytes. The padding keeps the last four-pixel read within its row.
    uint4 pixels = gBgra.Load4(y * gBgraPitch + x * 4);
    uint address = y * gPlanePitch + x;
    uint planeSize = gHeight * gPlanePitch;
    // HEVC identity matrix order is G, B, R. nvJPEG can also take these planes directly.
    gPlanes.Store(address, packChannel(pixels, 8));
    gPlanes.Store(address + planeSize, packChannel(pixels, 0));
    gPlanes.Store(address + 2 * planeSize, packChannel(pixels, 16));
}
