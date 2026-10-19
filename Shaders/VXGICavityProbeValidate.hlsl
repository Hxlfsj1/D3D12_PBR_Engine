// Test-only replay of finite world fallback rays. No production sampling,
// threshold, bias, geometry, cache, or validity policy is changed here.
#include "VXGIProbeRadianceCommon.hlsli"
#include "VXGIMeshSDFThresholdCommon.hlsli"

// 32 bytes, two 16-byte rows. Mesh buffers retain their production space1
// declarations: assets=t0, instances=t1, material IDs=t2, instance BVH=t3.
cbuffer CavityConstants : register(b0)
{
    uint nodeCount; uint assetCount; uint instanceCount; uint bvhNodeCount;
    float voxelSize; uint probeCount; uint queryCount; uint constantsPadding;
};

// 32 bytes. The caller defines origins and applies any intended SSGI normal bias;
// this shader never moves them or extends the original finite ray interval.
struct CavityQueryInput
{
    float3 worldOrigin; float rayLength;
    float3 worldDirection; uint padding;
};

// 144 bytes. Each nested production query result is exactly 48 bytes:
// float3 radiance + supportWeight; float3 unclampedRadiance + negativeMask;
// candidateCount, acceptedCount, hasSupport, traceFailures.
struct CavityQueryResult
{
    float3 hitPosition; float hitDistance;
    float fieldDistance; uint traceStatus; uint instanceIndex; uint instanceId;
    uint sdfSampleCount; uint modelCandidateCount; uint rayInputValid; uint padding;
    VXGIProbeRadianceQueryResult hitQuery;
    VXGIProbeRadianceQueryResult originQuery;
};

StructuredBuffer<float> nodes : register(t0);
StructuredBuffer<VXGIProbeCellEntry> lookup : register(t1);
StructuredBuffer<VXGIProbeRecord> raw : register(t2);
StructuredBuffer<CavityQueryInput> queries : register(t3);
RWStructuredBuffer<CavityQueryResult> results : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= queryCount) return;
    CavityQueryInput input = queries[id.x];
    CavityQueryResult result = (CavityQueryResult)0;
    result.traceStatus = VXGI_THRESHOLD_INVALID;
    result.instanceIndex = 0xffffffffu;

    // Keep an origin-cache control even for invalid trace inputs. The helper
    // independently reports an invalid position/direction through traceFailures.
    result.originQuery = VXGIProbeQueryRadiance(input.worldOrigin, input.worldDirection,
        voxelSize, probeCount, lookup, raw);
    float3 unitDirection;
    if (!all(isfinite(input.worldOrigin)) || !isfinite(input.rayLength)
        || input.rayLength <= 0.0
        || !VXGIThresholdNormalize(input.worldDirection, unitDirection))
    {
        results[id.x] = result;
        return;
    }
    result.rayInputValid = 1;
    result.originQuery = VXGIProbeQueryRadiance(input.worldOrigin, unitDirection,
        voxelSize, probeCount, lookup, raw);

    VXGIMeshSDFThresholdCounts counts;
    counts.nodeCount = nodeCount;
    counts.assetCount = assetCount;
    counts.instanceCount = instanceCount;
    counts.bvhNodeCount = bvhNodeCount;
    VXGIThresholdParameters parameters;
    parameters.hitThresholdCells = 0.9;
    parameters.stepScale = 0.9;
    parameters.maxSteps = 512;
    parameters.padding = 0;
    VXGIMeshSDFThresholdResult hit = VXGIMeshSDFTraceThreshold(input.worldOrigin,
        unitDirection, 0.0, input.rayLength, counts, parameters, nodes);
    result.hitPosition = hit.position;
    result.hitDistance = hit.distance;
    result.fieldDistance = hit.fieldDistance;
    result.traceStatus = hit.status;
    result.instanceIndex = hit.instanceIndex;
    result.instanceId = hit.instanceId;
    result.sdfSampleCount = hit.sampleCount;
    result.modelCandidateCount = hit.candidateCount;
    if (hit.status == VXGI_THRESHOLD_HIT)
        result.hitQuery = VXGIProbeQueryRadiance(hit.position, unitDirection,
            voxelSize, probeCount, lookup, raw);
    results[id.x] = result;
}
