#ifndef SSGI_COMMON_HLSLI
#define SSGI_COMMON_HLSLI

#include "MaterialCommon.hlsli"
#include "GeometryCommon.hlsli"

float3 SSGIDecodeViewNormal(float3 encodedNormal, float4x4 viewMat)
{
    return normalize(mul(float4(DecodeGBufferNormal(encodedNormal), 0), viewMat).xyz);
}

// Reconstruct/Composite share these geometry weights, but choose their own spatial
// kernel and rejection thresholds. Keep the multiplication order of the original filters.
float SSGIApplyBilateralGeometryWeight(float spatialWeight, float normalAgreement, float planeError)
{
    return spatialWeight * pow(normalAgreement, 32) * exp2(-4 * planeError * planeError);
}

bool SSGISameSurface(float3 p, float3 n, float3 q, float3 m, float planeTolerance)
{
    float3 delta = q - p;
    // Bilinear reprojection already bounds the taps. Coplanar grazing-angle taps
    // may be far apart along the surface, so test normals and tangent planes.
    return dot(n, m) >= 0.85 && SymmetricPlaneDistance(delta, n, m) <= planeTolerance;
}

// Trace computes this once for every receiver and caches the result for Temporal.
// Geometry always comes from this frame; only color and accumulation are reused.
bool SSGIReprojectHistory(uint2 pixel, uint2 sceneSize, uint2 outputSize, uint factor,
    float z, float3 n, float4x4 currInvViewProj, float4x4 prevViewProj, float4x4 prevInvViewProj,
    uint previousGITextureIdx, uint previousDepthTextureIdx, uint previousNormalTextureIdx,
    uint previousFastTextureIdx, out float4 history, out float4 fastHistory)
{
    history = 0;
    fastHistory = 0;
    float2 uv = (pixel + 0.5) / sceneSize;
    float3 p = ReconstructPosition(uv, z, currInvViewProj);
    float footprint = max(SameDepthPixelFootprint(uv, z, p, sceneSize, currInvViewProj), 1e-4);
    float rayDepth = length(p - ReconstructPosition(uv, 0, currInvViewProj));
    float planeTolerance = max(min(footprint * 0.5, rayDepth * 0.01), 0.001);
    float4 previousClip = mul(float4(p, 1), prevViewProj);
    if (previousClip.w <= 0)
    {
        return false;
    }
    float3 previousNDC = previousClip.xyz / previousClip.w;
    float2 previousUV = previousNDC.xy * float2(0.5, -0.5) + 0.5;
    if (any(previousUV < 0) || any(previousUV >= 1) || previousNDC.z < 0 || previousNDC.z >= 1)
    {
        return false;
    }

    // History texel i represents full pixel center factor*i+0.5.
    float inverseFactor = factor == 2u ? 0.5 : rcp(float(factor));
    float2 historyPixel = (previousUV * sceneSize - 0.5) * inverseFactor;
    int2 base = int2(floor(historyPixel));
    float2 fraction = frac(historyPixel);
    Texture2D<float4> previousGI = ResourceDescriptorHeap[previousGITextureIdx];
    Texture2D<float> previousDepth = ResourceDescriptorHeap[previousDepthTextureIdx];
    Texture2D<float4> previousNormal = ResourceDescriptorHeap[previousNormalTextureIdx];
    Texture2D<float4> previousFast = ResourceDescriptorHeap[previousFastTextureIdx];
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
        float3 q = ReconstructPosition((tap * int(factor) + 0.5) / sceneSize, tapZ, prevInvViewProj);
        float3 m = normalize(DecodeGBufferNormal(tapNormal.xyz));
        if (!SSGISameSurface(p, n, q, m, planeTolerance))
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
        return false;
    }
    history /= historyWeight;
    fastHistory /= historyWeight;
    return true;
}

#endif
