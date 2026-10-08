// Stages retain independent b0 layouts; select one with SSGI_PASS_<STAGE>.

#if defined(SSGI_PASS_RECONSTRUCT)

#include "SSGICommon.hlsli"

cbuffer SSGIReconstructConstants : register(b0)
{
    float4x4 viewMat;
    float4x4 invProjMat;
    uint2 sceneSize;
    uint2 outputSize;
    uint depthTextureIdx;
    uint normalTextureIdx;
    uint ormTextureIdx;
    uint rawTextureIdx;
    uint outputTextureIdx;
    uint3 padding;
};

[numthreads(8, 8, 1)]
void CSMain_Reconstruct(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= outputSize))
    {
        return;
    }
    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    Texture2D<float4> normals = ResourceDescriptorHeap[normalTextureIdx];
    Texture2D<float4> orm = ResourceDescriptorHeap[ormTextureIdx];
    Texture2D<float4> raw = ResourceDescriptorHeap[rawTextureIdx];
    RWTexture2D<float4> output = ResourceDescriptorHeap[outputTextureIdx];
    uint2 pixel = id.xy * 2;
    float z = depth.Load(int3(pixel, 0));
    output[id.xy] = 0;
    if (z >= 1 || orm.Load(int3(pixel, 0)).a < 0.5)
    {
        return;
    }

    float2 uv = (pixel + 0.5) / sceneSize;
    float3 p = ReconstructPosition(uv, z, invProjMat);
    float3 n = SSGIDecodeViewNormal(normals.Load(int3(pixel, 0)).xyz, viewMat);
    float footprint = SSGISameDepthPixelFootprint(uv, z, p, sceneSize, invProjMat);
    float planeTolerance = max(0.5 * footprint, 1e-4);

    // Binomial 5x5 kernel [1,4,6,4,1] in each axis. Gather more current-frame
    // samples without extending temporal persistence. Geometry weights preserve surface edges.
    // Keep the center unconditional for isolated surfaces/1x1 views.
    float4 sum = raw.Load(int3(id.xy, 0)) * 36;
    float weightSum = 36;

    [loop]
    for (int y = -2; y <= 2; ++y)
    {
        [loop]
        for (int x = -2; x <= 2; ++x)
        {
            if (x == 0 && y == 0)
            {
                continue;
            }
            int2 tap = int2(id.xy) + int2(x, y);
            if (any(tap < 0) || any(tap >= int2(outputSize)))
            {
                continue;
            }
            int2 fullPixel = tap * 2;
            float tapZ = depth.Load(int3(fullPixel, 0));
            if (tapZ >= 1 || orm.Load(int3(fullPixel, 0)).a < 0.5)
            {
                continue;
            }
            float3 tapN = SSGIDecodeViewNormal(normals.Load(int3(fullPixel, 0)).xyz, viewMat);

            float normalAgreement = saturate(dot(n, tapN));
            if (normalAgreement < 0.8)
            {
                continue;
            }
            float3 tapP = ReconstructPosition((fullPixel + 0.5) / sceneSize, tapZ, invProjMat);
            float3 delta = tapP - p;
            // Symmetric tangent-plane distance preserves sloped surfaces better than |z0-z1|.
            float planeError = SSGISymmetricPlaneDistance(delta, n, tapN) / planeTolerance;
            if (planeError >= 3)
            {
                continue;
            }
            float spatialWeight = (x == 0 ? 6 : (abs(x) == 1 ? 4 : 1)) * (y == 0 ? 6 : (abs(y) == 1 ? 4 : 1));
            float weight = SSGIApplyBilateralGeometryWeight(spatialWeight, normalAgreement, planeError);
            sum += raw.Load(int3(tap, 0)) * weight;
            weightSum += weight;
        }
    }
    // RGB remains E/pi; alpha is the filtered hit fraction.
    // Zero-radiance misses are valid samples, not holes. Never divide by hit fraction.
    output[id.xy] = sum / weightSum;
}

#elif defined(SSGI_PASS_ATROUS)

#include "SSGICommon.hlsli"
#include "../ThirdParty/NVIDIA/NRD/RelaxDiffuse.hlsli"
#include "../ThirdParty/NVIDIA/NRD/RelaxAtrous.hlsli"
#ifndef SSGI_ATROUS_FIRST
#define SSGI_ATROUS_FIRST 0
#endif
cbuffer SSGIAtrousConstants : register(b0)
{
    float4x4 viewMat;
    float4x4 invProjMat;
    uint2 sceneSize;
    uint2 outputSize;
    uint giTextureIdx;
    uint depthTextureIdx;
    uint normalTextureIdx;
    uint historyTextureIdx;
    uint outputTextureIdx;
    uint stepWidth;
    float phiLuminance;
    float depthThreshold;
};
// Both variants require at most a two-texel border (short-history 5x5 or step-2 3x3).
groupshared float4 signalTile[12][12];
groupshared float4 normalTile[12][12]; // view normal, history length (0 means invalid)
groupshared float3 positionTile[12][12];

[numthreads(8, 8, 1)]
void CSMain_Atrous(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID,
            uint lane : SV_GroupIndex)
{
    Texture2D<float4> gi = ResourceDescriptorHeap[giTextureIdx];
    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    Texture2D<float4> normal = ResourceDescriptorHeap[normalTextureIdx];
    Texture2D<float4> history = ResourceDescriptorHeap[historyTextureIdx];
    RWTexture2D<float4> output = ResourceDescriptorHeap[outputTextureIdx];
    for (uint i = lane; i < 144; i += 64)
    {
        uint2 local = uint2(i % 12, i / 12);
        int2 pixel = clamp(int2(group.xy * 8 + local) - 2, 0, int2(outputSize) - 1);
        int2 fullPixel = pixel * 2;
        float historyLength = history.Load(int3(pixel, 0)).a;
        float3 n = 0;
        float3 p = 0;
        if (historyLength > 0)
        {
            n = SSGIDecodeViewNormal(normal.Load(int3(fullPixel, 0)).xyz, viewMat);
            p = ReconstructPosition((fullPixel + 0.5) / sceneSize, depth.Load(int3(fullPixel, 0)), invProjMat);
        }
        signalTile[local.y][local.x] = gi.Load(int3(pixel, 0));
        normalTile[local.y][local.x] = float4(n, historyLength);
        positionTile[local.y][local.x] = p;
    }
    GroupMemoryBarrierWithGroupSync();
    if (any(id.xy >= outputSize))
    {
        return;
    }
    uint2 center = thread.xy + 2;
    float4 centerNormal = normalTile[center.y][center.x];
    if (centerNormal.a == 0)
    {
        output[id.xy] = 0;
        return;
    }
    float3 p = positionTile[center.y][center.x];
    float threshold = max(depthThreshold * abs(p.z), 1e-6);
    float4 value = signalTile[center.y][center.x];
    float centerL = RelaxLuminance(value.rgb);
    float variance = value.a;
    bool shortHistory = false;
#if SSGI_ATROUS_FIRST
    // RELAX: smooth color + second moment before deriving the variance used by edge stopping.
    float4 smoothed = 0;
    float smoothWeight = 0;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            int2 tap = int2(center) + int2(x, y);
            if (normalTile[tap.y][tap.x].a > 0)
            {
                float k = (x == 0 ? 0.5 : 0.25) * (y == 0 ? 0.5 : 0.25);
                smoothed += signalTile[tap.y][tap.x] * k;
                smoothWeight += k;
            }
        }
    }
    smoothed /= smoothWeight;
    float mean = RelaxLuminance(smoothed.rgb);
    variance = max(0, smoothed.a - mean * mean);
    shortHistory = centerNormal.a < 3;
#endif
    // NRD defaults: diffuse phi=2, lobe fraction=.5. Relax normals while history is young.
    float fraction = lerp(0.99, 0.5 / sqrt(float(stepWidth)), saturate(centerNormal.a / 5.0));
    if (shortHistory)
    {
        fraction = 0.5;
    }
    float inverseSigma = rcp(max(1e-4, phiLuminance * sqrt(max(variance, 0))));
    int radius = shortHistory ? 2 : 1;
    float3 sum = 0;
    float momentOrVariance = 0;
    float sumWeight = 0;

    [loop]
    for (int y = -radius; y <= radius; ++y)
    {
        [loop]
        for (int x = -radius; x <= radius; ++x)
        {
            int2 offset = int2(x, y) * int(stepWidth);
            int2 pixel = int2(id.xy) + offset;
            if (any(pixel < 0) || any(pixel >= int2(outputSize)))
            {
                continue;
            }
            int2 tap = int2(center) + offset;
            float4 tapNormal = normalTile[tap.y][tap.x];
            if (tapNormal.a == 0)
            {
                continue;
            }
            float4 c = signalTile[tap.y][tap.x];
            float kernel = shortHistory ? 1 : (x == 0 ? 0.44198 : 0.27901) * (y == 0 ? 0.44198 : 0.27901);
            float w = kernel * RelaxAtrousNormalWeight(centerNormal.xyz, tapNormal.xyz, fraction);
            w *= RelaxAtrousPlaneWeight(p, centerNormal.xyz, positionTile[tap.y][tap.x], threshold);
            if (!shortHistory)
            {
                w *= exp(-abs(centerL - RelaxLuminance(c.rgb)) * inverseSigma);
            }
            sum += c.rgb * w;
#if SSGI_ATROUS_FIRST
            // The first pass averages second moments; only then convert to variance.
            momentOrVariance += c.a * w;
#else
            // Later passes propagate variance using squared weights, not ordinary color weights.
            momentOrVariance += max(c.a, 0) * w * w;
#endif
            sumWeight += w;
        }
    }
    float3 result = sum / sumWeight; // Valid center always contributes a positive weight.
#if SSGI_ATROUS_FIRST
    float luma = RelaxLuminance(result);
    float outVariance = max(0, momentOrVariance / sumWeight - luma * luma);
    if (shortHistory)
    {
        outVariance *= max(1.0, 4.0 / (centerNormal.a + 1.0));
    }
#else
    float outVariance = momentOrVariance / (sumWeight * sumWeight);
#endif
    output[id.xy] = float4(result, outVariance);
}

#endif
