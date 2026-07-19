cbuffer HiZConstants : register(b0)
{
    uint srcTextureIdx;
    uint dstTextureIdx;
    uint srcWidth;
    uint srcHeight;
    uint dstWidth;
    uint dstHeight;
};

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 dstCoord = dispatchThreadId.xy;
    if (dstCoord.x >= dstWidth || dstCoord.y >= dstHeight)
    {
        return;
    }

    Texture2D<float> srcTexture = ResourceDescriptorHeap[srcTextureIdx];
    RWTexture2D<float> dstTexture = ResourceDescriptorHeap[dstTextureIdx];

    if (srcWidth == dstWidth && srcHeight == dstHeight)
    {
        dstTexture[dstCoord] = srcTexture.Load(int3(dstCoord, 0));
        return;
    }

    uint2 srcBase = dstCoord * 2;
    float maxDepth = 0.0f;

    [unroll]
    for (uint y = 0; y < 2; ++y)
    {
        [unroll]
        for (uint x = 0; x < 2; ++x)
        {
            uint2 srcCoord = srcBase + uint2(x, y);
            if (srcCoord.x < srcWidth && srcCoord.y < srcHeight)
            {
                maxDepth = max(maxDepth, srcTexture.Load(int3(srcCoord, 0)));
            }
        }
    }

    dstTexture[dstCoord] = maxDepth;
}
