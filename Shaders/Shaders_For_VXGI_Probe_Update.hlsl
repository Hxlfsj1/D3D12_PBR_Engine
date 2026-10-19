#include "VXGIProbeCommon.hlsli"
#ifdef VXGI_PROBE_SPARSE
#include "VXGIProbeDependency.hlsli"
struct VXGIProbeWorkItem { uint probeIndex, resetMode, generation, padding; };
struct VXGIProbeFeedback { VXGIProbeRecord record; uint dependencies[8]; };
StructuredBuffer<VXGIProbeWorkItem> probeWork : register(t7);
StructuredBuffer<uint> probeRemap : register(t8);
StructuredBuffer<VXGIProbeWorkItem> probeInvalidations : register(t9);
RWStructuredBuffer<VXGIProbeFeedback> probeFeedback : register(u1);
cbuffer ProbeDependencyConstants : register(b1)
{
    VXGIProbeDependencyDomain dependencyDomain;
    uint previousProbeCount; uint layoutChanged; uint invalidationCount; uint sparsePadding1;
};
#endif

cbuffer ProbeConstants : register(b0)
{
    VXGIProbeParameters probeParameters;
};

StructuredBuffer<float> sdfNodes : register(t0);
StructuredBuffer<uint> cellOffsets : register(t1);
StructuredBuffer<uint> triangleReferences : register(t2);
StructuredBuffer<VXGISDFTriangle> surfaceTriangles : register(t3);
StructuredBuffer<VXGIMaterialData> surfaceMaterials : register(t4);
StructuredBuffer<float3> probeCenters : register(t5);
StructuredBuffer<VXGIProbeRecord> previousRecords : register(t6);
RWStructuredBuffer<VXGIProbeRecord> nextRecords : register(u0);
SamplerState materialSampler : register(s0);

// Sparse production remaps every surviving center when topology changes. Legacy
// numerical fixtures retain their original full-resource reset entry contract.
[numthreads(64, 1, 1)]
void CSReset(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint probeIndex = dispatchThreadId.x;
    if (probeIndex >= probeParameters.probeCount) return;
#ifdef VXGI_PROBE_SPARSE
    uint oldIndex = probeRemap[probeIndex];
    if (oldIndex < previousProbeCount) nextRecords[probeIndex] = previousRecords[oldIndex];
    else nextRecords[probeIndex] = (VXGIProbeRecord)0;
#else
    nextRecords[probeIndex] = VXGIProbeResetRecord(previousRecords[probeIndex], probeParameters.resetMode);
#endif
}

#ifdef VXGI_PROBE_SPARSE
[numthreads(64, 1, 1)]
void CSLegacyReset(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    if (index >= probeParameters.probeCount) return;
    if (probeParameters.resetMode == 1 || index >= previousProbeCount) nextRecords[index] = (VXGIProbeRecord)0;
    else nextRecords[index] = VXGIProbeResetRecord(previousRecords[index], probeParameters.resetMode);
}
[numthreads(64, 1, 1)]
void CSInvalidate(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    if (index >= invalidationCount) return;
    VXGIProbeWorkItem work = probeInvalidations[index];
    nextRecords[work.probeIndex] = VXGIProbeResetRecord(nextRecords[work.probeIndex], work.resetMode);
}
#endif

[numthreads(64, 1, 1)]
void CSUpdate(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint batchIndex = dispatchThreadId.x;
    if (batchIndex >= min(probeParameters.updateCount, probeParameters.probeCount)) return;
    // A cyclic batch can reach the end of the occupied-cell center array.
    // CPU batches contain at most probeCount entries, so every UAV has one writer.
    uint probeIndex = (probeParameters.updateStart + batchIndex) % probeParameters.probeCount;
    VXGIProbeRecord previous;
    if (probeParameters.resetMode != 0) previous = nextRecords[probeIndex];
    else previous = previousRecords[probeIndex];
    nextRecords[probeIndex] = VXGIProbeUpdate(previous, probeCenters[probeIndex], probeIndex, probeParameters,
        sdfNodes, cellOffsets, triangleReferences, surfaceTriangles, surfaceMaterials, materialSampler);
}

// Production dispatch: one 64-lane group per selected probe. The original
// one-thread-per-probe CSUpdate remains a numerical reference above.
#ifdef VXGI_PROBE_SPARSE
groupshared uint parallelDependencies[512];
void VXGIProbeStoreFeedback(uint lane, uint batchIndex, uint probeIndex, uint dependencies[8])
{
    [unroll] for (uint word = 0; word < 8; ++word) parallelDependencies[lane * 8 + word] = dependencies[word];
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = 32; stride > 0; stride >>= 1)
    {
        if (lane < stride)
            [unroll] for (uint word = 0; word < 8; ++word)
                parallelDependencies[lane * 8 + word] |= parallelDependencies[(lane + stride) * 8 + word];
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0)
    {
        VXGIProbeFeedback result;
        result.record = nextRecords[probeIndex];
        [unroll] for (uint word = 0; word < 8; ++word) result.dependencies[word] = parallelDependencies[word];
        probeFeedback[batchIndex] = result;
    }
}

bool VXGIProbeTraceSourceTracked(float3 center, float3 direction, out float3 radiance, inout uint dependencies[8])
{
    radiance = 0.0;
    VXGISDFGrid grid = VXGIProbeMakeGrid(probeParameters);
    VXGISDFHit hit;
    bool found = VXGISDFTrace(center, direction, probeParameters.rayMinDistance, probeParameters.maxRayDistance,
        grid, sdfNodes, cellOffsets, triangleReferences, surfaceTriangles, surfaceMaterials,
        probeParameters.materialCount, materialSampler, probeParameters.materialMipLevel, hit);
    VXGIProbeDependencySegment(center, center + direction * (found ? hit.distance : probeParameters.maxRayDistance), dependencyDomain, dependencies);
    if (hit.traceStatus >= 2) return false;
    if (!found) return hit.traceStatus == 0;
    if (hit.triangleIndex >= probeParameters.triangleCount
        || VXGISDFHitMaterialId(hit,surfaceTriangles) >= probeParameters.materialCount) return false;
    // Full finite sun segment is conservative even behind an existing blocker.
    VXGIProbeDependencySegment(hit.position, hit.position + normalize(probeParameters.sunDirectionTowardLight)
        * probeParameters.maxRayDistance, dependencyDomain, dependencies);
    VXGISurfaceSource source = VXGIEvaluateSurfaceSource(hit, grid, sdfNodes, cellOffsets,
        triangleReferences, surfaceTriangles, surfaceMaterials, probeParameters.materialCount, materialSampler,
        probeParameters.materialMipLevel, probeParameters.sunDirectionTowardLight, probeParameters.sunRadiance,
        probeParameters.rayMinDistance, probeParameters.maxRayDistance);
    if (source.valid == 0 || !all(isfinite(source.lo0))) return false;
    radiance = source.lo0;
    return true;
}
#endif

groupshared VXGIProbeRecord parallelRecord;
groupshared float3 parallelCenter;
groupshared uint parallelClassificationValid;
groupshared uint parallelBackfaces[64];
groupshared float parallelMinimumDistance[64];
groupshared uint parallelAnyHit[64];
groupshared uint parallelFailure[64];
groupshared float3 parallelSH0[64];
groupshared float3 parallelSH1[64];
groupshared float3 parallelSH2[64];
groupshared float3 parallelSH3[64];

[numthreads(64, 1, 1)]
void CSUpdateParallel(uint3 groupId : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint batchIndex = groupId.x;
    // This branch is uniform across a whole group and precedes every barrier.
    if (batchIndex >= min(probeParameters.updateCount, probeParameters.probeCount)) return;
#ifdef VXGI_PROBE_SPARSE
    batchIndex += probeParameters.updateStart;
    VXGIProbeWorkItem work = probeWork[batchIndex];
    uint probeIndex = work.probeIndex;
    uint dependencies[8];
    [unroll] for (uint word = 0; word < 8; ++word) dependencies[word] = 0;
#else
    uint probeIndex = (probeParameters.updateStart + batchIndex) % probeParameters.probeCount;
#endif
    if (lane == 0)
    {
#ifdef VXGI_PROBE_SPARSE
        parallelRecord = nextRecords[probeIndex];
        parallelRecord = VXGIProbeResetRecord(parallelRecord, work.resetMode);
#else
        if (probeParameters.resetMode != 0) parallelRecord = nextRecords[probeIndex];
        else parallelRecord = previousRecords[probeIndex];
#endif
        parallelCenter = probeCenters[probeIndex];
    }
    GroupMemoryBarrierWithGroupSync();

    if (parallelRecord.state == VXGI_PROBE_UNCLASSIFIED)
    {
        uint classificationRayCount = VXGIProbeRayCount(probeParameters.classifyRays);
        float minimumDistance = isfinite(probeParameters.maxRayDistance)
            ? max(probeParameters.maxRayDistance, 0.0) : 0.0;
        if (lane == 0)
            parallelClassificationValid = VXGIProbeTraceParametersValid(parallelCenter, probeParameters)
                && isfinite(probeParameters.backfaceThreshold) ? 1 : 0;
        GroupMemoryBarrierWithGroupSync();

        uint backfaces = 0;
        uint anyHit = 0;
        uint failure = 0;
        VXGISDFGrid grid = VXGIProbeMakeGrid(probeParameters);
        if (parallelClassificationValid != 0)
        {
            [loop] for (uint pairIndex = lane; pairIndex < classificationRayCount / 2; pairIndex += 64)
            {
                float3 direction = VXGIProbeSampleDirection(pairIndex, classificationRayCount / 2, probeIndex, 0);
                [unroll] for (uint side = 0; side < 2; ++side)
                {
                    VXGISDFHit hit;
                    bool found = VXGISDFTrace(parallelCenter, side == 0 ? direction : -direction, 0.0,
                        probeParameters.maxRayDistance, grid, sdfNodes, cellOffsets, triangleReferences,
                        surfaceTriangles, surfaceMaterials, probeParameters.materialCount, materialSampler,
                        probeParameters.materialMipLevel, hit);
#ifdef VXGI_PROBE_SPARSE
                    float3 classificationDirection = side == 0 ? direction : -direction;
                    VXGIProbeDependencySegment(parallelCenter, parallelCenter + classificationDirection
                        * (found ? hit.distance : probeParameters.maxRayDistance), dependencyDomain, dependencies);
#endif
                    failure |= hit.traceStatus >= 2 ? 1 : 0;
                    if (found)
                    {
                        anyHit = 1;
                        backfaces += hit.backFace != 0 ? 1 : 0;
                        minimumDistance = min(minimumDistance, hit.distance);
                    }
                }
            }
        }
        parallelBackfaces[lane] = backfaces;
        parallelMinimumDistance[lane] = minimumDistance;
        parallelAnyHit[lane] = anyHit;
        parallelFailure[lane] = failure;
        GroupMemoryBarrierWithGroupSync();
        [unroll] for (uint stride = 32; stride > 0; stride >>= 1)
        {
            if (lane < stride)
            {
                parallelBackfaces[lane] += parallelBackfaces[lane + stride];
                parallelMinimumDistance[lane] = min(parallelMinimumDistance[lane], parallelMinimumDistance[lane + stride]);
                parallelAnyHit[lane] |= parallelAnyHit[lane + stride];
                parallelFailure[lane] |= parallelFailure[lane + stride];
            }
            GroupMemoryBarrierWithGroupSync();
        }
        if (lane == 0)
        {
            parallelRecord = (VXGIProbeRecord)0;
            parallelRecord.minHitDistance = parallelMinimumDistance[0];
            if (parallelClassificationValid == 0)
            {
                parallelRecord.state = VXGI_PROBE_DISABLED;
                parallelRecord.reason = VXGI_PROBE_REASON_CLASSIFY_TRACE_FAILURE;
            }
            else
            {
                parallelRecord.classificationRays = classificationRayCount;
                parallelRecord.backfaceRatio = float(parallelBackfaces[0]) / float(classificationRayCount);
                parallelRecord.state = VXGI_PROBE_VALID;
                if (parallelFailure[0] != 0)
                    parallelRecord.reason = VXGI_PROBE_REASON_CLASSIFY_TRACE_FAILURE;
                else if (parallelAnyHit[0] != 0 && parallelRecord.minHitDistance <= probeParameters.rayMinDistance)
                    parallelRecord.reason = VXGI_PROBE_REASON_UNSAFE_ORIGIN;
                else if (parallelRecord.backfaceRatio > saturate(probeParameters.backfaceThreshold))
                    parallelRecord.reason = VXGI_PROBE_REASON_BACKFACE;
                if (parallelRecord.reason != VXGI_PROBE_REASON_NONE) parallelRecord.state = VXGI_PROBE_DISABLED;
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
    // All lanes observe the same classification, so no lane exits a later barrier.
    if (parallelRecord.state != VXGI_PROBE_VALID)
    {
        if (lane == 0) nextRecords[probeIndex] = parallelRecord;
#ifdef VXGI_PROBE_SPARSE
        VXGIProbeStoreFeedback(lane, batchIndex, probeIndex, dependencies);
#endif
        return;
    }

    uint rayCount = VXGIProbeRayCount(probeParameters.raysPerProbe);
    uint updateIndex = VXGIProbeSaturatingAdd(parallelRecord.updateCount, 1);
    VXGIProbeRecord partialEstimate = (VXGIProbeRecord)0;
    bool traceFailure = !VXGIProbeTraceParametersValid(parallelCenter, probeParameters);
    [loop] for (uint pairIndex = lane; pairIndex < rayCount / 2 && !traceFailure; pairIndex += 64)
    {
        float3 direction = VXGIProbeSampleDirection(pairIndex, rayCount / 2, probeIndex, updateIndex);
        float3 positiveRadiance, negativeRadiance;
#ifdef VXGI_PROBE_SPARSE
        bool positiveValid = VXGIProbeTraceSourceTracked(parallelCenter, direction, positiveRadiance, dependencies);
        bool negativeValid = VXGIProbeTraceSourceTracked(parallelCenter, -direction, negativeRadiance, dependencies);
#else
        bool positiveValid = VXGIProbeTraceSource(parallelCenter, direction, probeParameters,
            sdfNodes, cellOffsets, triangleReferences, surfaceTriangles, surfaceMaterials, materialSampler, positiveRadiance);
        bool negativeValid = VXGIProbeTraceSource(parallelCenter, -direction, probeParameters,
            sdfNodes, cellOffsets, triangleReferences, surfaceTriangles, surfaceMaterials, materialSampler, negativeRadiance);
#endif
        traceFailure = !positiveValid || !negativeValid;
        if (!traceFailure)
            VXGIProbeAccumulatePair(partialEstimate, direction, positiveRadiance, negativeRadiance, rayCount);
    }
    parallelSH0[lane] = partialEstimate.sh[0].rgb;
    parallelSH1[lane] = partialEstimate.sh[1].rgb;
    parallelSH2[lane] = partialEstimate.sh[2].rgb;
    parallelSH3[lane] = partialEstimate.sh[3].rgb;
    parallelFailure[lane] = traceFailure ? 1 : 0;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = 32; stride > 0; stride >>= 1)
    {
        if (lane < stride)
        {
            parallelSH0[lane] += parallelSH0[lane + stride];
            parallelSH1[lane] += parallelSH1[lane + stride];
            parallelSH2[lane] += parallelSH2[lane + stride];
            parallelSH3[lane] += parallelSH3[lane + stride];
            parallelFailure[lane] |= parallelFailure[lane + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0)
    {
        if (parallelFailure[0] != 0)
        {
            VXGIProbeClearSH(parallelRecord);
            parallelRecord.state = VXGI_PROBE_DISABLED;
            parallelRecord.reason = VXGI_PROBE_REASON_SAMPLE_TRACE_FAILURE;
        }
        else
        {
            uint historyUpdates = min(VXGIProbeSaturatingAdd(parallelRecord.updateCount, 1),
                max(probeParameters.maxHistoryUpdates, 1));
            float newWeight = 1.0 / float(historyUpdates);
            parallelRecord.sh[0] = float4(lerp(parallelRecord.sh[0].rgb, parallelSH0[0], newWeight), 0.0);
            parallelRecord.sh[1] = float4(lerp(parallelRecord.sh[1].rgb, parallelSH1[0], newWeight), 0.0);
            parallelRecord.sh[2] = float4(lerp(parallelRecord.sh[2].rgb, parallelSH2[0], newWeight), 0.0);
            parallelRecord.sh[3] = float4(lerp(parallelRecord.sh[3].rgb, parallelSH3[0], newWeight), 0.0);
            parallelRecord.updateCount = VXGIProbeSaturatingAdd(parallelRecord.updateCount, 1);
            parallelRecord.sampleCount = VXGIProbeSaturatingAdd(parallelRecord.sampleCount, rayCount);
            parallelRecord.lastUpdateFrame = probeParameters.frameIndex;
            parallelRecord.reason = VXGI_PROBE_REASON_NONE;
        }
        nextRecords[probeIndex] = parallelRecord;
    }
#ifdef VXGI_PROBE_SPARSE
    VXGIProbeStoreFeedback(lane, batchIndex, probeIndex, dependencies);
#endif
}
