#include "SSGICommon.hlsli"

cbuffer Constants : register(b0)
{
    float4x4 InvProj;
    uint2 SourceSize, OutputSize;
    uint DepthIndex, OutputDepthIndex, FirstLevel, Padding;
};

// Bounds are positive view Z: (nearest, furthest). Empty cells use (1e20, 0).
// Each level is a separate resource so RDG needs no subresource state tracking.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= OutputSize)) return;
    Texture2D<float2> depths = ResourceDescriptorHeap[DepthIndex];
    RWTexture2D<float2> outputDepth = ResourceDescriptorHeap[OutputDepthIndex];
    float2 bounds = float2(1e20, 0);
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        uint2 p = id.xy * 2 + uint2(i & 1, i >> 1);
        float2 b = float2(1e20, 0);
        if (all(p < SourceSize))
        {
            b = depths.Load(int3(p, 0));
            if (FirstLevel)
            {
                float z = SSGIViewPosition((p + 0.5) / SourceSize, b.x, InvProj).z;
                b = b.x < 1.0 ? z.xx : float2(1e20, 0);
            }
        }
        bounds = float2(min(bounds.x, b.x), max(bounds.y, b.y));
    }
    outputDepth[id.xy] = bounds;
}
