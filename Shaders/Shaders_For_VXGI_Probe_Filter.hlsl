#include "VXGIProbeQueryCommon.hlsli"

cbuffer FilterConstants : register(b0)
{
    VXGIProbeQueryParameters queryParameters;
};

StructuredBuffer<float> sdfNodes : register(t0);
StructuredBuffer<uint> cellOffsets : register(t1);
StructuredBuffer<uint> triangleReferences : register(t2);
StructuredBuffer<VXGISDFTriangle> surfaceTriangles : register(t3);
StructuredBuffer<float3> probeCenters : register(t4);
StructuredBuffer<VXGIProbeCellEntry> probeLookup : register(t5);
StructuredBuffer<VXGIProbeNeighborRecord> probeNeighbors : register(t6);
StructuredBuffer<VXGIProbeRecord> rawRecords : register(t7);
StructuredBuffer<uint> visibilityRecords : register(t8);
StructuredBuffer<VXGIMaterialData> surfaceMaterials : register(t9);
SamplerState materialSampler : register(s0);
RWStructuredBuffer<uint> nextVisibility : register(u0);
RWStructuredBuffer<VXGIProbeRecord> filteredRecords : register(u1);

// Sparse production inputs are deliberately absent from legacy entry points.
struct VXGIFilterWork { uint probeIndex; uint resetMode; uint generation; uint padding; };
StructuredBuffer<uint> sparseFilterIndices : register(t10);
StructuredBuffer<VXGIFilterWork> sparseInvalidations : register(t11);
StructuredBuffer<uint> historyRemap : register(t12);
StructuredBuffer<VXGIProbeRecord> previousFiltered : register(t13);
StructuredBuffer<uint> previousVisibility : register(t14);
StructuredBuffer<VXGIProbeNeighborRecord> previousNeighbors : register(t15);

// Mode 1 invalidates geometric edges and filtered SH; mode 2 keeps edges and
// invalidates only filtered SH. A UAV barrier must precede visibility/filtering.
[numthreads(64, 1, 1)]
void CSResetFilter(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint probeIndex = dispatchThreadId.x;
    if (probeIndex >= queryParameters.probeCount || queryParameters.resetMode == 0) return;
    if (queryParameters.resetMode == 1) nextVisibility[probeIndex] = 0;
    filteredRecords[probeIndex] = (VXGIProbeRecord)0;
}

groupshared uint groupVisibility;

// Dispatch one group per selected probe. Only six lanes trace; every lane joins
// both barriers. Coverage edits invalidate edges; ordinary lighting changes keep them.
void VXGIComputeVisibility(uint probeIndex, uint lane)
{
    if (lane == 0)
    {
        uint previous = nextVisibility[probeIndex];
        groupVisibility = (previous & VXGI_PROBE_VISIBILITY_KNOWN) != 0 ? previous : 0;
    }
    GroupMemoryBarrierWithGroupSync();
    if ((groupVisibility & VXGI_PROBE_VISIBILITY_KNOWN) != 0) return;
    if (lane < 6)
    {
        uint neighborIndex = probeNeighbors[probeIndex].indices[lane];
        uint flag = 0;
        if (neighborIndex != VXGI_PROBE_MISSING_INDEX)
        {
            bool traceFailure = true;
            bool visible = false;
            if (neighborIndex < queryParameters.probeCount && neighborIndex != probeIndex)
            {
                uint start = min(probeIndex, neighborIndex);
                uint end = max(probeIndex, neighborIndex);
                visible = VXGIProbeCenterVisibility(probeCenters[start], probeCenters[end],
                    VXGIProbeQueryMakeGrid(queryParameters), sdfNodes, cellOffsets,
                    triangleReferences, surfaceTriangles, surfaceMaterials, queryParameters.materialCount,
                    materialSampler, queryParameters.materialMipLevel, traceFailure);
            }
            if (visible) flag |= 1u << lane;
            if (traceFailure) flag |= 1u << (lane + 8);
        }
        if (flag != 0) InterlockedOr(groupVisibility, flag);
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0) nextVisibility[probeIndex] = groupVisibility | VXGI_PROBE_VISIBILITY_KNOWN;
}
[numthreads(64, 1, 1)]
void CSVisibility(uint3 groupId : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint batchIndex = groupId.x;
    if (batchIndex >= min(queryParameters.updateCount, queryParameters.probeCount)) return;
    VXGIComputeVisibility((queryParameters.updateStart + batchIndex) % queryParameters.probeCount, lane);
}

// This pass reads raw, never its own filtered output. Visibility must have its
// UAV barrier/transition to SRV before this separate dispatch.
[numthreads(64, 1, 1)]
void CSFilter(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint batchIndex = dispatchThreadId.x;
    if (batchIndex >= min(queryParameters.updateCount, queryParameters.probeCount)) return;
    uint probeIndex = (queryParameters.updateStart + batchIndex) % queryParameters.probeCount;
    filteredRecords[probeIndex] = VXGIProbeFilter(rawRecords[probeIndex], probeIndex, queryParameters,
        probeCenters, probeNeighbors, rawRecords, visibilityRecords);
}

// Topology events move records by stable world-cell identity. The mask can be
// reused only when every directional neighbor still denotes the same old cell.
[numthreads(64, 1, 1)]
void CSRemapFilter(uint3 id : SV_DispatchThreadID)
{
    uint index = id.x + queryParameters.updateStart;
    if (id.x >= queryParameters.updateCount || index >= queryParameters.probeCount) return;
    uint oldIndex = historyRemap[index];
    VXGIProbeRecord record = (VXGIProbeRecord)0;
    uint mask = 0;
    if (oldIndex < queryParameters.padding2)
    {
        record = previousFiltered[oldIndex];
        bool unchanged = true;
        [unroll] for (uint direction = 0; direction < 6; ++direction)
        {
            uint neighbor = probeNeighbors[index].indices[direction];
            uint mapped = neighbor < queryParameters.probeCount ? historyRemap[neighbor] : VXGI_PROBE_MISSING_INDEX;
            unchanged = unchanged && mapped == previousNeighbors[oldIndex].indices[direction];
        }
        if (unchanged) mask = previousVisibility[oldIndex];
    }
    filteredRecords[index] = record;
    nextVisibility[index] = mask;
}
[numthreads(64, 1, 1)]
void CSResetSparseFilter(uint3 id : SV_DispatchThreadID)
{
    uint item = id.x + queryParameters.updateStart;
    if (id.x >= queryParameters.updateCount) return;
    VXGIFilterWork work = sparseInvalidations[item];
    if (work.probeIndex >= queryParameters.probeCount) return;
    filteredRecords[work.probeIndex] = (VXGIProbeRecord)0;
    if (work.resetMode == 1) nextVisibility[work.probeIndex] = 0;
}
[numthreads(64, 1, 1)]
void CSVisibilitySparse(uint3 groupId : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint item = groupId.x + queryParameters.updateStart;
    if (groupId.x >= queryParameters.updateCount) return;
    uint index = sparseFilterIndices[item];
    if (index < queryParameters.probeCount) VXGIComputeVisibility(index, lane);
}
[numthreads(64, 1, 1)]
void CSFilterSparse(uint3 id : SV_DispatchThreadID)
{
    uint item = id.x + queryParameters.updateStart;
    if (id.x >= queryParameters.updateCount) return;
    uint index = sparseFilterIndices[item];
    if (index >= queryParameters.probeCount) return;
    filteredRecords[index] = VXGIProbeFilter(rawRecords[index], index, queryParameters,
        probeCenters, probeNeighbors, rawRecords, visibilityRecords);
}
