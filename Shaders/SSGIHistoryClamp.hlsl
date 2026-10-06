#include "../ThirdParty/NVIDIA/NRD/RelaxDiffuse.hlsli"

cbuffer Constants : register(b0)
{
    uint2 Size;
    uint SlowIndex, FastIndex;
    uint NoisyIndex, OutputGIIndex, OutputFastIndex;
    float SigmaScale;
    float AccelerationAmount, ResetAmount, SpatialSigmaScale, TemporalSigmaScale;
    uint OutputMomentsIndex;
    uint3 Padding;
};

// 8x8 workgroup plus a 2-pixel border. All threads preload before any early return.
groupshared float4 FastTile[12][12];
groupshared float4 NoisyTile[12][12];

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID,
    uint3 thread : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    Texture2D<float4> slowTexture = ResourceDescriptorHeap[SlowIndex];
    Texture2D<float4> fastTexture = ResourceDescriptorHeap[FastIndex];
    Texture2D<float4> noisyTexture = ResourceDescriptorHeap[NoisyIndex];
    RWTexture2D<float4> output = ResourceDescriptorHeap[OutputGIIndex];
    RWTexture2D<float4> outputMoments = ResourceDescriptorHeap[OutputMomentsIndex];
    RWTexture2D<float4> outputFast = ResourceDescriptorHeap[OutputFastIndex];
    for (uint i = lane; i < 144; i += 64)
    {
        uint2 local = uint2(i % 12, i / 12);
        int2 pixel = clamp(int2(group.xy * 8 + local) - 2, 0, int2(Size) - 1);
        float4 fast = fastTexture.Load(int3(pixel, 0));
        FastTile[local.y][local.x] = float4(RelaxToYCoCg(fast.rgb), fast.a);
        NoisyTile[local.y][local.x] = float4(noisyTexture.Load(int3(pixel, 0)).rgb, slowTexture.Load(int3(pixel, 0)).a > 0);
    }
    GroupMemoryBarrierWithGroupSync();
    if (any(id.xy >= Size)) return;
    uint2 center = thread.xy + 2;
    output[id.xy] = 0;
    outputFast[id.xy] = 0;
    outputMoments[id.xy] = 0;
    if (NoisyTile[center.y][center.x].a == 0) return;

    float3 fastMean = 0, fastMoment2 = 0, noisyMean = 0;
    float noisyMoment2 = 0, count = 0;
    // RELAX's clamp statistics use in-range samples, not the strict reprojection surface test.
    [unroll] for (int y = -2; y <= 2; ++y)
    [unroll] for (int x = -2; x <= 2; ++x)
    {
        uint2 tap = uint2(int2(center) + int2(x, y));
        float4 noisy = NoisyTile[tap.y][tap.x];
        if (noisy.a != 0)
        {
            float3 fast = FastTile[tap.y][tap.x].rgb;
            fastMean += fast; fastMoment2 += fast * fast;
            noisyMean += noisy.rgb;
            float luma = RelaxLuminance(noisy.rgb);
            noisyMoment2 += luma * luma;
            count += 1;
        }
    }
    float4 slow = slowTexture.Load(int3(id.xy, 0));
    float3 fast = RelaxToRGB(FastTile[center.y][center.x].rgb);
    float3 result;
    RelaxClampDiffuse(slow.rgb, fast, NoisyTile[center.y][center.x].rgb, slow.a,
        fastMean / count, fastMoment2 / count, noisyMean / count, noisyMoment2 / count,
        SigmaScale, AccelerationAmount, ResetAmount, SpatialSigmaScale, TemporalSigmaScale, result);
    // Nonnegative radiance is the local SSGI history/composite contract.
    result = max(result, 0);
    // RELAX second-moment correction preserves variance after history color changes.
    float oldL = RelaxLuminance(slow.rgb), newL = RelaxLuminance(result);
    float moment2 = max(0, FastTile[center.y][center.x].a + newL * newL - oldL * oldL);
    output[id.xy] = float4(result, slow.a);
    outputFast[id.xy] = float4(max(fast, 0), moment2);
    outputMoments[id.xy] = float4(result, moment2);
}
