// Stages retain independent b0 layouts; select one with SSGI_PASS_<STAGE>.

#if defined(SSGI_PASS_TEMPORAL)

#include "SSGICommon.hlsli"
#include "MaterialCommon.hlsli"
#include "../ThirdParty/NVIDIA/NRD/RelaxDiffuse.hlsli"

cbuffer SSGITemporalConstants : register(b0)
{
    uint2 sceneSize;
    uint2 outputSize;
    uint depthTextureIdx;
    uint normalTextureIdx;
    uint ormTextureIdx;
    uint currentTextureIdx;
    uint reprojectedGITextureIdx;
    uint reprojectedFastTextureIdx;
    uint outputGITextureIdx;
    uint outputDepthTextureIdx;
    uint outputNormalTextureIdx;
    uint outputFastTextureIdx;
    float maxHistoryFrames;
    float maxFastAccumulatedFrames;
    uint traceDownsampleFactor;
    uint3 padding;
};

[numthreads(8, 8, 1)]
void CSMain_Temporal(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= outputSize))
    {
        return;
    }

    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    Texture2D<float4> normal = ResourceDescriptorHeap[normalTextureIdx];
    Texture2D<float4> orm = ResourceDescriptorHeap[ormTextureIdx];
    Texture2D<float4> current = ResourceDescriptorHeap[currentTextureIdx];
    RWTexture2D<float4> outputFast = ResourceDescriptorHeap[outputFastTextureIdx];
    RWTexture2D<float4> output = ResourceDescriptorHeap[outputGITextureIdx];
    RWTexture2D<float> outputDepth = ResourceDescriptorHeap[outputDepthTextureIdx];
    RWTexture2D<float4> outputNormal = ResourceDescriptorHeap[outputNormalTextureIdx];
    uint factor = max(traceDownsampleFactor, 2u);
    uint2 pixel = id.xy * factor;
    float z = depth.Load(int3(pixel, 0));
    output[id.xy] = 0;
    outputFast[id.xy] = 0;
    outputDepth[id.xy] = 1;
    outputNormal[id.xy] = 0;
    if (z >= 1 || orm.Load(int3(pixel, 0)).a < 0.5)
    {
        return;
    }

    float3 encodedNormal = normal.Load(int3(pixel, 0)).xyz;
    float4 currentSample = current.Load(int3(id.xy, 0));
    float3 value = currentSample.rgb;
    outputDepth[id.xy] = z;
    outputNormal[id.xy] = float4(encodedNormal, 1);
    float luminance = RelaxLuminance(value);
    float moment2 = luminance * luminance;
    outputFast[id.xy] = float4(value, moment2);
    output[id.xy] = float4(value, 1); // Alpha is history length here, not the raw hit fraction.

    Texture2D<float4> reprojectedGI = ResourceDescriptorHeap[reprojectedGITextureIdx];
    float4 history = reprojectedGI.Load(int3(id.xy, 0));
    if (history.a <= 0)
    {
        return;
    }
    Texture2D<float4> reprojectedFast = ResourceDescriptorHeap[reprojectedFastTextureIdx];
    float4 fastHistory = reprojectedFast.Load(int3(id.xy, 0));
    if (currentSample.a < 0)
    {
        // No new observation: retain accumulated color, moments and sample count.
        // Depth and normal above still describe the current frame's surface.
        output[id.xy] = history;
        outputFast[id.xy] = fastHistory;
        return;
    }
    float frames = min(history.a + 1, maxHistoryFrames);
    // RELAX: independent slow/fast accumulation before neighborhood clamping.
    float alpha = 1.0 / frames;
    float fastAlpha = max(1.0 / (maxFastAccumulatedFrames + 1.0), alpha);
    output[id.xy] = float4(lerp(history.rgb, value, alpha), frames);
    outputFast[id.xy] = float4(lerp(fastHistory.rgb, value, fastAlpha), lerp(fastHistory.a, moment2, alpha));
}

#elif defined(SSGI_PASS_HISTORYCLAMP)

#include "../ThirdParty/NVIDIA/NRD/RelaxDiffuse.hlsli"

cbuffer SSGIHistoryClampConstants : register(b0)
{
    uint2 signalSize;
    uint slowTextureIdx;
    uint fastTextureIdx;
    uint noisyTextureIdx;
    uint outputGITextureIdx;
    uint outputFastTextureIdx;
    float sigmaScale;
    float accelerationAmount;
    float resetAmount;
    float spatialSigmaScale;
    float temporalSigmaScale;
    uint outputMomentsTextureIdx;
    uint3 padding;
};

// 8x8 workgroup plus a 2-pixel border. All threads preload before any early return.
groupshared float4 fastTile[12][12];
groupshared float4 noisyTile[12][12];

[numthreads(8, 8, 1)]
void CSMain_HistoryClamp(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID,
            uint lane : SV_GroupIndex)
{
    Texture2D<float4> slowTexture = ResourceDescriptorHeap[slowTextureIdx];
    Texture2D<float4> fastTexture = ResourceDescriptorHeap[fastTextureIdx];
    Texture2D<float4> noisyTexture = ResourceDescriptorHeap[noisyTextureIdx];
    RWTexture2D<float4> output = ResourceDescriptorHeap[outputGITextureIdx];
    RWTexture2D<float4> outputMoments = ResourceDescriptorHeap[outputMomentsTextureIdx];
    RWTexture2D<float4> outputFast = ResourceDescriptorHeap[outputFastTextureIdx];
    for (uint i = lane; i < 144; i += 64)
    {
        uint2 local = uint2(i % 12, i / 12);
        int2 pixel = clamp(int2(group.xy * 8 + local) - 2, 0, int2(signalSize) - 1);
        float4 fast = fastTexture.Load(int3(pixel, 0));
        fastTile[local.y][local.x] = float4(RGBToYCoCg(fast.rgb), fast.a);
        float4 noisy = noisyTexture.Load(int3(pixel, 0));
        float sampleState = slowTexture.Load(int3(pixel, 0)).a > 0 ? (noisy.a < 0 ? -1 : 1) : 0;
        noisyTile[local.y][local.x] = float4(noisy.rgb, sampleState);
    }
    GroupMemoryBarrierWithGroupSync();
    if (any(id.xy >= signalSize))
    {
        return;
    }
    uint2 center = thread.xy + 2;
    output[id.xy] = 0;
    outputFast[id.xy] = 0;
    outputMoments[id.xy] = 0;
    if (noisyTile[center.y][center.x].a == 0)
    {
        return;
    }
    if (noisyTile[center.y][center.x].a < 0)
    {
        // Reuse is not a new noisy observation to clamp against. Preserve both
        // histories and pass the retained second moment to the spatial filter.
        float4 slow = slowTexture.Load(int3(id.xy, 0));
        float4 fast = fastTexture.Load(int3(id.xy, 0));
        output[id.xy] = slow;
        outputFast[id.xy] = fast;
        outputMoments[id.xy] = float4(slow.rgb, fast.a);
        return;
    }

    float3 fastMean = 0;
    float3 fastMoment2 = 0;
    float3 noisyMean = 0;
    float noisyMoment2 = 0;
    float count = 0;
    // RELAX's clamp statistics use in-range samples, not the strict reprojection surface test.

    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            uint2 tap = uint2(int2(center) + int2(x, y));
            float4 noisy = noisyTile[tap.y][tap.x];
            if (noisy.a > 0)
            {
                float3 fast = fastTile[tap.y][tap.x].rgb;
                fastMean += fast;
                fastMoment2 += fast * fast;
                noisyMean += noisy.rgb;
                float luma = RelaxLuminance(noisy.rgb);
                noisyMoment2 += luma * luma;
                count += 1;
            }
        }
    }
    float4 slow = slowTexture.Load(int3(id.xy, 0));
    float3 fast = YCoCgToRGB(fastTile[center.y][center.x].rgb);
    float3 result;
    RelaxClampDiffuse(slow.rgb, fast, noisyTile[center.y][center.x].rgb, slow.a, fastMean / count, fastMoment2 / count,
                      noisyMean / count, noisyMoment2 / count, sigmaScale, accelerationAmount, resetAmount,
                      spatialSigmaScale, temporalSigmaScale, result);
    // Nonnegative radiance is the local SSGI history/composite contract.
    result = max(result, 0);
    // RELAX second-moment correction preserves variance after history color changes.
    float oldL = RelaxLuminance(slow.rgb);
    float newL = RelaxLuminance(result);
    float moment2 = max(0, fastTile[center.y][center.x].a + newL * newL - oldL * oldL);
    output[id.xy] = float4(result, slow.a);
    outputFast[id.xy] = float4(max(fast, 0), moment2);
    outputMoments[id.xy] = float4(result, moment2);
}

#endif
