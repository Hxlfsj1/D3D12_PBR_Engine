#ifndef SSGI_WORLD_OWNER_CACHE_HLSLI
#define SSGI_WORLD_OWNER_CACHE_HLSLI

// Spare uint4 components preserve the trace constant-buffer ABI. Both raw
// owner atlases pack Mesh on the left and Global on the right, each GI/2 wide.
// State stores successful fresh-batch count (at least First/Mature) and stamp.
// With history > 1 the legacy raw atlas stores accumulated owner estimates;
// the capture metadata explicitly records this filtered-feedback contract.
#define previousGlobalOwnerRawIdx levels[3].z
#define previousGlobalOwnerStateIdx levels[3].w
#define previousGlobalOwnerReceiverIdx levels[4].y
#define currentGlobalOwnerStateIdx levels[4].z
#define globalDisplayTextureIdx levels[5].y
#define worldOwnerCacheEnabled levels[6].z
#define worldOwnerHistoryUpdates levels[5].z

uint SSGIWorldOwnerHistoryUpdates() { return max(worldOwnerHistoryUpdates, 1u); }

bool SSGIWorldOwnerCacheEnabled()
{
    return worldOwnerCacheEnabled != 0u;
}

// Pick one geometrically valid owner without interpolating its raw estimate,
// first-batch state or source origin. Expired observations still transport the
// update state needed to schedule the next fresh batch.
bool SSGIReprojectWorldOwner(uint2 receiverGI, uint span, uint band,
    out float4 raw, out float2 state, out uint sourceOwner)
{
    raw = float4(0, 0, 0, -1);
    state = 0;
    sourceOwner = 0u;
    if (historyValid == 0u) return false;
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
    int2 base = int2(floor(historyPixel / 2.0));
    uint2 atlasSize = (outputSize + 1u) / 2u;
    int2 bandOffset = int2(band == 2u ? atlasSize.x : 0u, 0);
    Texture2D<uint2> addresses = ResourceDescriptorHeap[previousGlobalOwnerReceiverIdx];
    Texture2D<float4> values = ResourceDescriptorHeap[previousGlobalOwnerRawIdx];
    Texture2D<float2> updates = ResourceDescriptorHeap[previousGlobalOwnerStateIdx];
    Texture2D<float> previousDepth = ResourceDescriptorHeap[previousDepthTextureIdx];
    Texture2D<float4> previousNormals = ResourceDescriptorHeap[previousNormalTextureIdx];
    float bestDistance = 1e20;
    [unroll] for (uint i = 0; i < 4u; ++i)
    {
        int2 tile = base + int2(i & 1u, i >> 1u);
        if (any(tile < 0) || any(tile >= int2(atlasSize))) continue;
        int2 packedTile = tile + bandOffset;
        uint2 owner = addresses.Load(int3(packedTile, 0));
        uint address = owner.x;
        if (address == 0u || owner.y == 0u || SSGIAddressSpan(address) != span) continue;
        uint2 tapGI = SharedReceiverCoordinate(address);
        if (any(tapGI >= outputSize)) continue;
        float2 receiverDelta = float2(tapGI) - historyPixel;
        float distanceSquared = dot(receiverDelta, receiverDelta);
        if (distanceSquared >= bestDistance) continue;
        float4 normal = previousNormals.Load(int3(tapGI, 0));
        if (normal.a < 0.5) continue;
        float previousZ = previousDepth.Load(int3(tapGI, 0));
        float3 q = ReconstructPosition((tapGI * factor + 0.5) / sceneSize,
            previousZ, prevJitteredInvViewProj);
        if (!SSGISameSurface(p, n, q, normalize(DecodeGBufferNormal(normal.xyz)), tolerance)) continue;
        float4 candidate = values.Load(int3(packedTile, 0));
        float2 candidateState = updates.Load(int3(packedTile, 0));
        if (!all(isfinite(candidate)) || candidate.a < 0 || !all(isfinite(candidateState)) || candidateState.x < 1) continue;
        raw = float4(candidate.rgb, candidate.a + 1.0);
        state = candidateState;
        sourceOwner = owner.y;
        bestDistance = distanceSquared;
    }
    return raw.a >= 0;
}

// Spread the first refresh of cold owners, then use the original 2/4-frame
// deadline. Mesh/Global use matching low hash bits to retain world-prefix reuse.
// Age and batch stamps always remain the real age of a traced observation.
bool SSGIWorldOwnerRefreshDue(float4 raw, float2 state, uint sourceOwner, uint period)
{
    if (raw.a >= float(period)) return true;
    if (state.x != 1.0 || sourceOwner == 0u) return false;
    // Keep each SharedWorld groups coherent while distributing 32x32 GI batches.
    uint2 tile = SharedReceiverCoordinate(sourceOwner) / 32u;
    uint phaseHash = (tile.x * 0x9e3779b9u) ^ (tile.y * 0x85ebca6bu);
    phaseHash ^= phaseHash >> 16u;
    uint firstInterval = 1u + (phaseHash & (period - 1u));
    return raw.a >= float(firstInterval);
}

float2 SSGIAdvanceWorldOwnerState(float2 state)
{
    float stamp = float((frameIndex & 65535u) + 1u);
    if (state.x >= 1 && state.y == stamp) return state;
    // Advance only on a new traced batch. Reprojection and replicated slots do not count.
    return float2(min(max(state.x, 0) + 1.0, float(max(SSGIWorldOwnerHistoryUpdates(), 2u))), stamp);
}
#endif
