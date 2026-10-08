// Stages retain independent b0 layouts; select one with SSGI_PASS_<STAGE>.

#if defined(SSGI_PASS_TEMPORAL)

#include "SSGICommon.hlsli"
#include "MaterialCommon.hlsli"
#include "../ThirdParty/NVIDIA/NRD/RelaxDiffuse.hlsli"

cbuffer SSGITemporalConstants : register(b0)
{
    float4x4 currJitteredInvViewProj;
    float4x4 prevJitteredViewProj;
    float4x4 prevJitteredInvViewProj;
    uint2 sceneSize;
    uint2 outputSize;
    uint depthTextureIdx;
    uint normalTextureIdx;
    uint ormTextureIdx;
    uint currentTextureIdx;
    uint previousGITextureIdx;
    uint previousDepthTextureIdx;
    uint previousNormalTextureIdx;
    uint historyValid;
    uint outputGITextureIdx;
    uint outputDepthTextureIdx;
    uint outputNormalTextureIdx;
    uint previousFastTextureIdx;
    uint outputFastTextureIdx;
    float maxHistoryFrames;
    float maxFastAccumulatedFrames;
    uint padding;
};

bool SameSurface(float3 p, float3 n, float3 q, float3 m, float planeTolerance)
{
    float3 delta = q - p;
    // Reprojection already restricts taps to the previous bilinear footprint.
    // At grazing angles coplanar taps can be far apart along the surface;
    // reject normal/plane disagreement, not that tangential separation.
    return dot(n, m) >= 0.85 && SSGISymmetricPlaneDistance(delta, n, m) <= planeTolerance;
}

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
    uint2 pixel = id.xy * 2;
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
    float3 n = normalize(DecodeGBufferNormal(encodedNormal));
    float3 value = current.Load(int3(id.xy, 0)).rgb;
    outputDepth[id.xy] = z;
    outputNormal[id.xy] = float4(encodedNormal, 1);
    float luminance = RelaxLuminance(value);
    float moment2 = luminance * luminance;
    outputFast[id.xy] = float4(value, moment2);
    output[id.xy] = float4(value, 1); // Alpha is history length here, not the raw hit fraction.

    if (!historyValid)
    {
        return;
    }

    float2 uv = (pixel + 0.5) / sceneSize;
    float3 p = ReconstructPosition(uv, z, currJitteredInvViewProj);
    float footprint = SSGISameDepthPixelFootprint(uv, z, p, sceneSize, currJitteredInvViewProj);
    footprint = max(footprint, 1e-4);
    float rayDepth = length(p - ReconstructPosition(uv, 0, currJitteredInvViewProj));
    float planeTolerance = max(min(footprint * 0.5, rayDepth * 0.01), 0.001);
    float4 previousClip = mul(float4(p, 1), prevJitteredViewProj);
    if (previousClip.w <= 0)
    {
        return;
    }
    float3 previousNDC = previousClip.xyz / previousClip.w;
    float2 previousUV = previousNDC.xy * float2(0.5, -0.5) + 0.5;
    if (any(previousUV < 0) || any(previousUV >= 1) || previousNDC.z < 0 || previousNDC.z >= 1)
    {
        return;
    }
    // History texel i represents full pixel center 2*i+0.5, not the center of a 2x2 block.
    float2 historyPixel = (previousUV * sceneSize - 0.5) * 0.5;
    int2 base = int2(floor(historyPixel));
    float2 fraction = frac(historyPixel);
    Texture2D<float4> previousGI = ResourceDescriptorHeap[previousGITextureIdx];
    Texture2D<float> previousDepth = ResourceDescriptorHeap[previousDepthTextureIdx];
    Texture2D<float4> previousNormal = ResourceDescriptorHeap[previousNormalTextureIdx];

    Texture2D<float4> previousFast = ResourceDescriptorHeap[previousFastTextureIdx];
    float4 fastHistory = 0;
    float4 history = 0;
    float historyWeight = 0;

    [loop]
    for (int i = 0; i < 4; ++i)
    {
        int2 offset = int2(i & 1, i >> 1);
        int2 tap = base + offset;
        if (any(tap < 0) || any(tap >= int2(outputSize)))
        {
            continue;
        }

        float4 tapNormal = previousNormal.Load(int3(tap, 0));
        if (tapNormal.a < 0.5)
        {
            continue;
        }
        float tapZ = previousDepth.Load(int3(tap, 0));
        float3 q = ReconstructPosition((tap * 2 + 0.5) / sceneSize, tapZ, prevJitteredInvViewProj);
        float3 m = normalize(DecodeGBufferNormal(tapNormal.xyz));

        if (!SameSurface(p, n, q, m, planeTolerance))
        {
            continue;
        }

        float2 weights = lerp(1 - fraction, fraction, float2(offset));
        float weight = weights.x * weights.y;

        history += previousGI.Load(int3(tap, 0)) * weight;
        fastHistory += previousFast.Load(int3(tap, 0)) * weight;
        historyWeight += weight;
    }

    if (historyWeight < 0.01)
    {
        return;
    }

    history /= historyWeight;

    fastHistory /= historyWeight;
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
        noisyTile[local.y][local.x] =
            float4(noisyTexture.Load(int3(pixel, 0)).rgb, slowTexture.Load(int3(pixel, 0)).a > 0);
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
            if (noisy.a != 0)
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
