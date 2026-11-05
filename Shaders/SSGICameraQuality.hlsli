#ifndef SSGI_CAMERA_QUALITY_HLSLI
#define SSGI_CAMERA_QUALITY_HLSLI

// Screen history remains independent. These kinds describe only world-band
// estimates; the policy texture keeps layout separate from RGB and cache age.
static const uint SSGI_WORLD_CACHE_INVALID = 0u;
static const uint SSGI_WORLD_CACHE_FINE = 1u;
static const uint SSGI_WORLD_CACHE_COARSE = 2u;
static const uint SSGI_WORLD_CACHE_POINT = 3u;

uint SSGIWorldSpan(uint policy)
{
    return (policy & 1u) != 0u ? 4u : 2u;
}

uint SSGIWorldKind(uint span)
{
    return span == 4u ? SSGI_WORLD_CACHE_COARSE : SSGI_WORLD_CACHE_FINE;
}

uint SSGIBandKind(uint policy, uint band)
{
    uint shift = band == 1u ? 1u : 3u;
    return (policy >> shift) & 3u;
}

uint SSGISetBandKind(uint policy, uint band, uint kind)
{
    uint shift = band == 1u ? 1u : 3u;
    return (policy & ~(3u << shift)) | ((kind & 3u) << shift);
}

uint SSGIPolicyForSpan(uint span)
{
    return span == 4u ? 1u : 0u;
}

uint SSGIWorldAddress(uint address, uint span)
{
    if (address == 0u) return 0u;
    return (address & 0x7fffffffu) | (span == 4u ? 0x80000000u : 0u);
}

uint SSGIAddressSpan(uint address)
{
    return (address & 0x80000000u) != 0u ? 4u : 2u;
}

uint SSGICameraSpan(uint2 receiverGI)
{
    uint factor = SSGITraceFactor(traceDownsampleFactor);
    uint2 pixel = receiverGI * factor;
    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    float z = depth.Load(int3(pixel, 0));
    float2 uv = (pixel + 0.5) / sceneSize;
    float3 viewPosition = ReconstructPosition(uv, z, invProjMat);
    float distanceSquared = dot(viewPosition, viewPosition);
    float nearSquared = asfloat(levels[2].y);
    float exitSquared = asfloat(levels[2].z);
    if (distanceSquared <= nearSquared) return 2u;
    if (distanceSquared > exitSquared || historyValid == 0) return 4u;
    if (!isfinite(z) || z < 0 || z >= 1 || !all(isfinite(viewPosition))) return 4u;

    // Only the hysteresis shell needs previous policy. Its screen coordinates
    // must reproject onto the same surface, rather than inherit a screen tile.
    Texture2D<float4> normals = ResourceDescriptorHeap[normalTextureIdx];
    float3 p = ReconstructPosition(uv, z, invViewProjMat);
    float3 n = normalize(DecodeGBufferNormal(normals.Load(int3(pixel, 0)).xyz));
    float footprint = max(SameDepthPixelFootprint(uv, z, p, sceneSize, invViewProjMat), 1e-4);
    float rayDepth = length(p - ReconstructPosition(uv, 0, invViewProjMat));
    float tolerance = max(min(footprint * 0.5, rayDepth * 0.01), 0.001);
    float4 clip = mul(float4(p, 1), prevJitteredViewProj);
    if (clip.w <= 0) return 4u;
    float3 ndc = clip.xyz / clip.w;
    float2 previousUV = ndc.xy * float2(0.5, -0.5) + 0.5;
    if (any(previousUV < 0) || any(previousUV >= 1) || ndc.z < 0 || ndc.z >= 1) return 4u;
    float2 historyPixel = (previousUV * sceneSize - 0.5) / float(factor);
    int2 base = int2(floor(historyPixel));
    float2 fraction = frac(historyPixel);
    Texture2D<float> previousDepth = ResourceDescriptorHeap[previousDepthTextureIdx];
    Texture2D<float4> previousNormals = ResourceDescriptorHeap[previousNormalTextureIdx];
    Texture2D<uint> previousPolicy = ResourceDescriptorHeap[previousWorldPolicyTextureIdx];
    float strongestWeight = 0;
    uint inheritedSpan = 4u;
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        int2 offset = int2(i & 1u, i >> 1u);
        int2 tap = base + offset;
        if (any(tap < 0) || any(tap >= int2(outputSize))) continue;
        float2 weights = lerp(1 - fraction, fraction, float2(offset));
        float weight = weights.x * weights.y;
        if (weight <= strongestWeight || weight < 1e-6) continue;
        float4 normal = previousNormals.Load(int3(tap, 0));
        if (normal.a < 0.5) continue;
        float previousZ = previousDepth.Load(int3(tap, 0));
        float3 q = ReconstructPosition((tap * int(factor) + 0.5) / sceneSize,
            previousZ, prevJitteredInvViewProj);
        if (!SSGISameSurface(p, n, q, normalize(DecodeGBufferNormal(normal.xyz)), tolerance)) continue;
        strongestWeight = weight;
        inheritedSpan = SSGIWorldSpan(previousPolicy.Load(int3(tap, 0)));
    }
    return inheritedSpan;
}

bool ReprojectWorldBand(uint2 receiverGI, uint previousIndex, uint period,
    uint expectedSpan, bool allowPoint, out float4 value, out uint kind)
{
    value = float4(0, 0, 0, -1);
    kind = SSGI_WORLD_CACHE_INVALID;
    if (historyValid == 0) return false;
    uint factor = SSGITraceFactor(traceDownsampleFactor);
    uint2 pixel = receiverGI * factor;
    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    Texture2D<float4> normals = ResourceDescriptorHeap[normalTextureIdx];
    float z = depth.Load(int3(pixel, 0));
    float2 uv = (pixel + 0.5) / sceneSize;
    float3 p = ReconstructPosition(uv, z, invViewProjMat);
    float3 n = normalize(DecodeGBufferNormal(normals.Load(int3(pixel, 0)).xyz));
    float footprint = max(SameDepthPixelFootprint(uv, z, p, sceneSize, invViewProjMat), 1e-4);
    float rayDepth = length(p - ReconstructPosition(uv, 0, invViewProjMat));
    float tolerance = max(min(footprint * 0.5, rayDepth * 0.01), 0.001);
    float4 clip = mul(float4(p, 1), prevJitteredViewProj);
    if (clip.w <= 0) return false;
    float3 ndc = clip.xyz / clip.w;
    float2 previousUV = ndc.xy * float2(0.5, -0.5) + 0.5;
    if (any(previousUV < 0) || any(previousUV >= 1) || ndc.z < 0 || ndc.z >= 1) return false;
    float2 historyPixel = (previousUV * sceneSize - 0.5) / float(factor);
    int2 base = int2(floor(historyPixel));
    float2 fraction = frac(historyPixel);
    Texture2D<float4> previousBand = ResourceDescriptorHeap[previousIndex];
    Texture2D<float> previousDepth = ResourceDescriptorHeap[previousDepthTextureIdx];
    Texture2D<float4> previousNormals = ResourceDescriptorHeap[previousNormalTextureIdx];
    Texture2D<uint> previousPolicy = ResourceDescriptorHeap[previousWorldPolicyTextureIdx];
    uint band = previousIndex == previousMeshBandIndex ? 1u : 2u;
    uint expectedKind = SSGIWorldKind(expectedSpan);
    bool hasPoint = false;
    bool hasShared = false;
    float3 sum = 0;
    float weightSum = 0, maxAge = 0;
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        int2 offset = int2(i & 1u, i >> 1u);
        int2 tap = base + offset;
        if (any(tap < 0) || any(tap >= int2(outputSize))) continue;
        float2 weights = lerp(1 - fraction, fraction, float2(offset));
        float weight = weights.x * weights.y;
        if (weight < 1e-6) continue;
        float4 normal = previousNormals.Load(int3(tap, 0));
        if (normal.a < 0.5) continue;
        float previousZ = previousDepth.Load(int3(tap, 0));
        float3 q = ReconstructPosition((tap * int(factor) + 0.5) / sceneSize,
            previousZ, prevJitteredInvViewProj);
        if (!SSGISameSurface(p, n, q, normalize(DecodeGBufferNormal(normal.xyz)), tolerance)) continue;
        uint tapKind = SSGIBandKind(previousPolicy.Load(int3(tap, 0)), band);
        if (tapKind != expectedKind && !(allowPoint && tapKind == SSGI_WORLD_CACHE_POINT)) return false;
        bool isPoint = tapKind == SSGI_WORLD_CACHE_POINT;
        // A mixed estimate cannot be labelled as a layout-independent point:
        // that would allow its shared part to cross a later quality change.
        if ((isPoint && hasShared) || (!isPoint && hasPoint)) return false;
        float4 cache = previousBand.Load(int3(tap, 0));
        if (!all(isfinite(cache)) || cache.a < 0 ||
            (period != 0u && cache.a >= float(period - 1u))) return false;
        hasPoint = hasPoint || isPoint;
        hasShared = hasShared || !isPoint;
        sum += cache.rgb * weight;
        weightSum += weight;
        maxAge = max(maxAge, cache.a);
    }
    if (weightSum < 0.01) return false;
    value = float4(sum / weightSum, maxAge + 1.0);
    kind = hasPoint ? SSGI_WORLD_CACHE_POINT : expectedKind;
    return true;
}

#endif
