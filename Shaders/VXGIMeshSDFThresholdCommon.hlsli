#ifndef VXGI_MESH_SDF_THRESHOLD_COMMON_HLSLI
#define VXGI_MESH_SDF_THRESHOLD_COMMON_HLSLI

#include "VXGIMeshSDFData.hlsli"
#include "VXGISDFThresholdCommon.hlsli"

// Preorder stackless tree; UINT_MAX instanceIndex identifies interior nodes.
// All bounds are tight world model bounds, not the padded SDF lattice.
struct VXGIMeshSDFBVHNode
{
    float3 boundsMinimum; uint instanceIndex;
    float3 boundsMaximum; uint escapeIndex;
};

StructuredBuffer<VXGIMeshSDFBVHNode> VXGIMeshSDFBVH : register(t3, space1);

// Root SRVs do not provide descriptor element counts. The caller supplies the
// immutable node/asset sizes and the current instance/BVH sizes explicitly.
struct VXGIMeshSDFThresholdCounts
{
    uint nodeCount;
    uint assetCount;
    uint instanceCount;
    uint bvhNodeCount;
};

struct VXGIMeshSDFThresholdCandidate
{
    uint instanceIndex;
    uint instanceId;
    float entryDistance;
    float exitDistance;
};

struct VXGIMeshSDFThresholdCandidates
{
    uint valid;
    uint count;
    VXGIMeshSDFThresholdCandidate candidates[2];
};

struct VXGIMeshSDFThresholdResult
{
    uint status;
    float3 position;
    float distance;
    float fieldDistance;
    uint instanceIndex;
    uint instanceId;
    uint sampleCount;
    uint candidateCount;
};

bool VXGIMeshSDFThresholdBounds(float3 origin, float3 unitDirection,
    float3 minimum, float3 maximum, float rayMin, float rayMax,
    out float entry, out float exit)
{
    entry = rayMin;
    exit = rayMax;
    [unroll] for (uint axis = 0; axis < 3; ++axis)
    {
        if (unitDirection[axis] == 0.0)
        {
            if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis]) return false;
        }
        else
        {
            float a = (minimum[axis] - origin[axis]) / unitDirection[axis];
            float b = (maximum[axis] - origin[axis]) / unitDirection[axis];
            entry = max(entry, min(a, b));
            exit = min(exit, max(a, b));
        }
    }
    return entry <= exit;
}

bool VXGIMeshSDFThresholdBefore(VXGIMeshSDFThresholdCandidate a,
    VXGIMeshSDFThresholdCandidate b)
{
    if (a.entryDistance != b.entryDistance) return a.entryDistance < b.entryDistance;
    if (a.instanceId != b.instanceId) return a.instanceId < b.instanceId;
    return a.instanceIndex < b.instanceIndex;
}

// The direction is already normalized by the world query. Select by AABB
// entry, preserving ties regardless of the median tree's traversal order.
VXGIMeshSDFThresholdCandidates VXGIMeshSDFThresholdClosestTwo(
    float3 origin, float3 unitDirection, float rayMin, float rayMax,
    VXGIMeshSDFThresholdCounts counts)
{
    VXGIMeshSDFThresholdCandidates result = (VXGIMeshSDFThresholdCandidates)0;
    if ((counts.instanceCount == 0) != (counts.bvhNodeCount == 0)) return result;
    uint index = 0;
    [loop] while (index < counts.bvhNodeCount)
    {
        VXGIMeshSDFBVHNode node = VXGIMeshSDFBVH[index];
        // The CPU produces a validated immutable traversal layout. Guard its
        // escape links so a stale or malformed upload cannot loop indefinitely.
        if (node.escapeIndex <= index || node.escapeIndex > counts.bvhNodeCount) return result;
        float entry, exit;
        if (!VXGIMeshSDFThresholdBounds(origin, unitDirection,
            node.boundsMinimum, node.boundsMaximum, rayMin, rayMax, entry, exit) ||
            (result.count == 2 && entry > result.candidates[1].entryDistance))
        {
            index = node.escapeIndex;
            continue;
        }
        if (node.instanceIndex != 0xffffffffu)
        {
            if (node.instanceIndex >= counts.instanceCount) return result;
            VXGIMeshSDFThresholdCandidate candidate;
            candidate.instanceIndex = node.instanceIndex;
            candidate.instanceId = VXGIMeshSDFInstances[node.instanceIndex].instanceId;
            candidate.entryDistance = entry;
            candidate.exitDistance = exit;
            if (result.count == 0)
            {
                result.candidates[0] = candidate;
                result.count = 1;
            }
            else if (VXGIMeshSDFThresholdBefore(candidate, result.candidates[0]))
            {
                result.candidates[1] = result.candidates[0];
                result.candidates[0] = candidate;
                result.count = 2;
            }
            else if (result.count == 1 || VXGIMeshSDFThresholdBefore(candidate, result.candidates[1]))
            {
                result.candidates[1] = candidate;
                result.count = 2;
            }
        }
        ++index;
    }
    result.valid = 1;
    return result;
}

VXGIMeshSDFThresholdResult VXGIMeshSDFTraceThreshold(float3 origin,
    float3 direction, float rayMin, float rayMax, VXGIMeshSDFThresholdCounts counts,
    VXGIThresholdParameters parameters, StructuredBuffer<float> nodes)
{
    VXGIMeshSDFThresholdResult result = (VXGIMeshSDFThresholdResult)0;
    result.status = VXGI_THRESHOLD_INVALID;
    result.instanceIndex = 0xffffffffu;
    float3 unitDirection;
    if (!all(isfinite(origin)) || !isfinite(rayMin) || !isfinite(rayMax) ||
        rayMin < 0.0 || rayMax < rayMin ||
        !isfinite(parameters.hitThresholdCells) ||
        parameters.hitThresholdCells <= VXGI_THRESHOLD_MAX_INTERPOLATION_ERROR_CELLS ||
        !isfinite(parameters.stepScale) || parameters.stepScale <= 0.0 ||
        parameters.stepScale > 1.0 || parameters.maxSteps == 0 ||
        !VXGIThresholdNormalize(direction, unitDirection)) return result;
    VXGIMeshSDFThresholdCandidates candidates = VXGIMeshSDFThresholdClosestTwo(
        origin, unitDirection, rayMin, rayMax, counts);
    if (candidates.valid == 0) return result;
    result.status = VXGI_THRESHOLD_MISS;
    result.candidateCount = candidates.count;
    uint incompleteStatus = VXGI_THRESHOLD_MISS;
    float incompleteEntry = 3.402823466e+38;
    [loop] for (uint candidateIndex = 0; candidateIndex < candidates.count; ++candidateIndex)
    {
        VXGIMeshSDFThresholdCandidate candidate = candidates.candidates[candidateIndex];
        float entry = max(rayMin, min(rayMax, candidate.entryDistance));
        float exit = max(entry, min(rayMax, candidate.exitDistance));
        if (result.status == VXGI_THRESHOLD_HIT && entry > result.distance) continue;
        VXGIMeshSDFInstance instance = VXGIMeshSDFInstances[candidate.instanceIndex];
        if (instance.assetIndex >= counts.assetCount)
        {
            result.status = VXGI_THRESHOLD_INVALID;
            return result;
        }
        VXGIMeshSDFAsset asset = VXGIMeshSDFAssets[instance.assetIndex];
        float3 localOrigin = mul(float4(origin, 1.0), instance.localFromWorld).xyz;
        float3 localVector = mul(float4(unitDirection, 0.0), instance.localFromWorld).xyz;
        float largest = max(abs(localVector.x), max(abs(localVector.y), abs(localVector.z)));
        if (!isfinite(largest) || largest <= 0.0)
        {
            result.status = VXGI_THRESHOLD_INVALID;
            return result;
        }
        float3 scaled = localVector / largest;
        float distanceScale = largest * sqrt(dot(scaled, scaled));
        if (!isfinite(distanceScale) || distanceScale <= 0.0)
        {
            result.status = VXGI_THRESHOLD_INVALID;
            return result;
        }
        VXGIThresholdGrid grid;
        grid.origin = asset.origin;
        grid.cellSize = asset.cellSize;
        grid.cellDimensions = asset.cellDimensions;
        grid.nodeOffset = asset.nodeOffset;
        grid.boundsMaximum = asset.boundsMaximum;
        grid.padding = 0;
        float maximum = result.status == VXGI_THRESHOLD_HIT ? min(exit, result.distance) : exit;
        VXGIThresholdResult local = VXGIThresholdTraceLocal(nodes, counts.nodeCount, grid,
            localOrigin, localVector, entry * distanceScale, maximum * distanceScale, parameters);
        result.sampleCount += local.sampleCount;
        if (local.status == VXGI_THRESHOLD_INVALID)
        {
            result.status = VXGI_THRESHOLD_INVALID;
            return result;
        }
        if (local.status == VXGI_THRESHOLD_EXHAUSTED || local.status == VXGI_THRESHOLD_STALLED)
        {
            if (entry < incompleteEntry)
            {
                incompleteEntry = entry;
                incompleteStatus = local.status;
            }
            continue;
        }
        if (local.status != VXGI_THRESHOLD_HIT) continue;
        float distance = max(entry, min(maximum, local.distance / distanceScale));
        if (result.status == VXGI_THRESHOLD_HIT && (distance > result.distance ||
            (distance == result.distance && (candidate.instanceId > result.instanceId ||
            (candidate.instanceId == result.instanceId && candidate.instanceIndex >= result.instanceIndex))))) continue;
        result.status = VXGI_THRESHOLD_HIT;
        result.position = origin + unitDirection * distance;
        result.distance = distance;
        result.fieldDistance = local.fieldDistance;
        result.instanceIndex = candidate.instanceIndex;
        result.instanceId = candidate.instanceId;
    }
    // An unfinished nearer model cannot become a proven miss or allow a
    // farther approximate hit to masquerade as the nearest valid result.
    if (incompleteStatus != VXGI_THRESHOLD_MISS &&
        (result.status != VXGI_THRESHOLD_HIT || incompleteEntry <= result.distance))
    {
        result.status = incompleteStatus;
        result.position = 0.0;
        result.distance = 0.0;
        result.fieldDistance = 0.0;
        result.instanceIndex = 0xffffffffu;
        result.instanceId = 0;
    }
    return result;
}

#endif
