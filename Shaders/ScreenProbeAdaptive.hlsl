// Included by ScreenProbeGather.hlsl. Adapted from UE 5.8.4
// LumenScreenProbeGather.usf: GetAdaptiveSampleCoord, MarkCS, SpawnCS and
// SetupAdaptiveProbeIndirectArgsCS. Eight samples per tile; group allocation,
// lower-sample-index coverage rejection and per-tile adaptive lists are retained.

uint2 AdaptiveSampleCoord(uint2 tile, uint sample)
{
    uint factor = SSGITraceFactor(traceDownsampleFactor);
    uint2 first = tile * factor, size = min(factor.xx, sceneSize - first);
    uint seed = ProbeRandomHash(tile.x + tile.y * outputSize.x + frameIndex * 0x9e3779b9u);
    float2 shift = float2(seed & 65535u, seed >> 16u) / 65536.0;
    float2 hammersley = float2(float(sample) / float(SP_ADAPTIVE_SAMPLES),
        float(reversebits(sample)) * 2.3283064365386963e-10);
    return first + min(uint2(frac(hammersley + shift) * size), size - 1u);
}

bool AdaptiveMaterial(uint2 pixel, out float3 p, out float3 n, out float footprint)
{
    p = 0; n = 0; footprint = 0;
    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    Texture2D<float4> orm = ResourceDescriptorHeap[ormTextureIdx];
    Texture2D<float4> normals = ResourceDescriptorHeap[normalTextureIdx];
    float z = depth.Load(int3(pixel, 0));
    if (!isfinite(z) || z < 0 || z >= 1 || orm.Load(int3(pixel, 0)).a < .5) return false;
    p = ReconstructPosition((pixel + .5) / sceneSize, z, invViewProjMat);
    n = SSGIReceiverGeometricNormal(pixel, normalize(DecodeGBufferNormal(normals.Load(int3(pixel, 0)).xyz)));
    if (!all(isfinite(p)) || !all(isfinite(n))) return false;
    float3 camera = mul(float4(0, 0, 0, 1), invViewMat).xyz;
    if (probeCameraFadeEnabled != 0u && distance(p, camera) >= probeCameraFadeEnd) return false;
    footprint = ProbeFootprint(pixel, p);
    return true;
}

[numthreads(8, 8, 1)]
void CSMain_ProbeClear(uint3 id : SV_DispatchThreadID)
{
    if (adaptiveCapacity > 0u)
    {
        if (all(id.xy == 0u))
        {
            RWTexture2D<uint> count = ResourceDescriptorHeap[adaptiveCountUavIndex];
            count[uint2(0, 0)] = 0u;
        }
        if (all(id.xy < outputSize))
        {
            RWTexture2D<uint> header = ResourceDescriptorHeap[adaptiveHeaderUavIndex];
            header[id.xy] = 0u;
        }
    }
    if (probeDiagnosticsIndexPlusOne != 0u && id.x < 8u && id.y < 4u)
    {
        RWTexture2D<uint> counters = ResourceDescriptorHeap[probeDiagnosticsIndexPlusOne - 1u];
        counters[uint2(id.y * 8u + id.x, 0)] = 0u;
    }
}

groupshared uint adaptivePlacementMasks[8];

[numthreads(8, 8, 1)]
void CSMain_AdaptiveMark(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    if (thread.x == 0u) adaptivePlacementMasks[thread.y] = 0u;
    GroupMemoryBarrierWithGroupSync();
    uint2 tile = uint2(group.x, group.y * 8u + thread.y);
    if (all(tile < outputSize))
    {
        uint2 pixel = AdaptiveSampleCoord(tile, thread.x);
        float3 p, n; float footprint;
        if (AdaptiveMaterial(pixel, p, n, footprint))
        {
            SPCounter(probeDiagnosticsIndexPlusOne, 0u, 1u);
            ScreenProbeSamples support = SPSelectSamples(ProbeQuery(), pixel, p, n, footprint, false);
            if (dot(support.weights, 1.0.xxxx) < SP_MIN_INTERPOLATION_WEIGHT)
            {
                InterlockedOr(adaptivePlacementMasks[thread.y], 1u << thread.x);
                SPCounter(probeDiagnosticsIndexPlusOne, 1u, 1u);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (thread.x == 0u && all(tile < outputSize))
    {
        RWTexture2D<uint> mask = ResourceDescriptorHeap[adaptiveMaskUavIndex];
        mask[tile] = adaptivePlacementMasks[thread.y];
    }
}

groupshared uint adaptiveGroupCount, adaptiveGroupBase;

[numthreads(8, 8, 1)]
void CSMain_AdaptiveSpawn(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    if (lane == 0u) adaptiveGroupCount = 0u;
    GroupMemoryBarrierWithGroupSync();
    uint2 tile = uint2(group.x, group.y * 8u + thread.y);
    uint sample = thread.x;
    bool place = false;
    uint2 pixel = 0;
    float3 p = 0, n = 0; float footprint = 0;
    Texture2D<uint> mask = ResourceDescriptorHeap[adaptiveMaskIndex];
    if (all(tile < outputSize) && (mask.Load(int3(tile, 0)) & (1u << sample)) != 0u)
    {
        pixel = AdaptiveSampleCoord(tile, sample);
        place = AdaptiveMaterial(pixel, p, n, footprint);
        float4 cornerWeights = 0;
        int2 base = int2(floor((pixel + .5) / float(SSGITraceFactor(traceDownsampleFactor)) - .5));
        [loop] for (uint corner = 0u; corner < 4u && place; ++corner)
        {
            int2 neighbor = base + int2(corner & 1u, corner >> 1u);
            if (any(neighbor < 0) || any(neighbor >= int2(outputSize))) continue;
            uint bits = mask.Load(int3(neighbor, 0));
            [loop] while (bits != 0u)
            {
                uint lowerSample = firstbitlow(bits);
                bits &= bits - 1u;
                if (lowerSample >= sample) break;
                uint2 otherPixel = AdaptiveSampleCoord(uint2(neighbor), lowerSample);
                float3 other, otherN; float otherFootprint;
                if (AdaptiveMaterial(otherPixel, other, otherN, otherFootprint))
                {
                    float w = SPGeometryWeight(p, n, other, otherN, footprint) *
                        SPAdaptiveSpatialWeight(pixel, otherPixel, SSGITraceFactor(traceDownsampleFactor));
                    cornerWeights[corner] = max(cornerWeights[corner], w);
                    if (dot(cornerWeights, 1.0.xxxx) >= SP_MIN_INTERPOLATION_WEIGHT)
                    { place = false; SPCounter(probeDiagnosticsIndexPlusOne, 2u, 1u); break; }
                }
            }
        }
    }
    uint local = 0u;
    if (place) InterlockedAdd(adaptiveGroupCount, 1u, local);
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0u)
    {
        RWTexture2D<uint> count = ResourceDescriptorHeap[adaptiveCountUavIndex];
        InterlockedAdd(count[uint2(0, 0)], adaptiveGroupCount, adaptiveGroupBase);
        SPCounter(probeDiagnosticsIndexPlusOne, 3u, adaptiveGroupCount);
    }
    GroupMemoryBarrierWithGroupSync();
    uint adaptiveID = adaptiveGroupBase + local;
    if (place && adaptiveID < adaptiveCapacity)
    {
        uint id = outputSize.x * outputSize.y + adaptiveID;
        uint2 coord = SPAtlasCoord(id, probeAtlasSize);
        RWTexture2D<float4> positions = ResourceDescriptorHeap[SP_POSITION_UAV];
        RWTexture2D<float4> normals = ResourceDescriptorHeap[SP_NORMAL_UAV];
        RWTexture2D<float4> anchors = ResourceDescriptorHeap[SP_ANCHOR_UAV];
        RWTexture2D<uint> receivers = ResourceDescriptorHeap[SP_RECEIVER_UAV];
        positions[coord] = float4(p, 1); normals[coord] = float4(n, 0);
        anchors[coord] = float4(p, 1); receivers[coord] = pixel.y * sceneSize.x + pixel.x + 1u;
        RWTexture2D<uint> header = ResourceDescriptorHeap[adaptiveHeaderUavIndex];
        RWTexture2D<uint> indices = ResourceDescriptorHeap[adaptiveIndicesUavIndex];
        uint slot; InterlockedAdd(header[tile], 1u, slot);
        if (slot < SP_ADAPTIVE_SAMPLES) indices[uint2(tile.x * SP_ADAPTIVE_SAMPLES + slot, tile.y)] = id;
        SPCounter(probeDiagnosticsIndexPlusOne, 4u, 1u);
    }
    else if (place) SPCounter(probeDiagnosticsIndexPlusOne, 5u, 1u);
}

[numthreads(1, 1, 1)]
void CSMain_AdaptiveArgs(uint3 id : SV_DispatchThreadID)
{
    uint extra = SPAdaptiveCount(ProbeQuery());
    uint count = outputSize.x * outputSize.y + extra;
    RWStructuredBuffer<uint> args = ResourceDescriptorHeap[adaptiveArgsUavIndex];
    args[0] = probeAtlasSize.x; args[1] = (count + probeAtlasSize.x - 1u) / probeAtlasSize.x; args[2] = 1u;
    args[3] = extra > 0u ? probeAtlasSize.x : 0u;
    args[4] = (extra + probeAtlasSize.x - 1u) / probeAtlasSize.x; args[5] = 1u;
}
