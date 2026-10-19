// Finite real-scene acceptance diagnostics. Production helpers define coverage,
// first-hit identity and query acceptance; additional counters explain support.
#include "VXGIProbeQueryCommon.hlsli"
cbuffer FilterConstants : register(b0) { VXGIProbeQueryParameters parameters; };
cbuffer ValidationConstants : register(b1) { uint queryCount; uint padding0; uint padding1; uint padding2; };
StructuredBuffer<float> nodes : register(t0);
StructuredBuffer<uint> offsets : register(t1);
StructuredBuffer<uint> references : register(t2);
StructuredBuffer<VXGISDFTriangle> triangles : register(t3);
StructuredBuffer<float3> centers : register(t4);
StructuredBuffer<VXGIProbeCellEntry> lookup : register(t5);
StructuredBuffer<VXGIProbeNeighborRecord> neighbors : register(t6);
StructuredBuffer<VXGIProbeRecord> raw : register(t7);
StructuredBuffer<uint> visibility : register(t8);
StructuredBuffer<VXGIMaterialData> materials : register(t9);
SamplerState materialSampler : register(s0);
struct QueryInput
{
    float3 position; uint triangleIndex;
    float3 geometricNormal; uint backFace;
    float3 mappedNormal; uint valid;
    float3 barycentric; uint triangleId;
    float distance; uint traceStatus; uint padding0; uint padding1;
};
StructuredBuffer<QueryInput> queries : register(t10);
struct SupportResult
{
    VXGIProbeQueryResult query;
    // 1: verified covered primary, 2: hole, 3: invalid coverage, 4: primary miss,
    // 5: different first triangle/endpoint, 6: trace failure, 7: invalid material.
    uint status; uint coverageStatus; uint primaryTraceStatus; uint primaryTriangleIndex;
    uint positiveCorners; uint missingCorners; uint disabledCorners; uint unclassifiedCorners;
    uint neverCapturedCorners; uint nonfiniteCorners; uint eligibleCorners; uint hemisphereRejectedCorners;
    uint visibilityRejectedCorners; uint visibilityFailures; uint acceptedCorners; uint padding;
};
RWStructuredBuffer<SupportResult> results : register(u2);
[numthreads(64,1,1)]
void CSQuery(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= queryCount) return;
    QueryInput input = queries[id.x];
    SupportResult result = (SupportResult)0;
    result.primaryTriangleIndex = 0xffffffffu;
    VXGISDFTriangle target = triangles[input.triangleIndex];
    result.coverageStatus = VXGISDFCandidateCoverage(target, input.barycentric,
        materials, parameters.materialCount, materialSampler, parameters.materialMipLevel);
    if (result.coverageStatus != 1)
    {
        result.status = result.coverageStatus == 0 ? 2u : 3u;
        results[id.x] = result; return;
    }
    VXGISDFGrid grid = VXGIProbeQueryMakeGrid(parameters);
    float3 towardOrigin = normalize(input.geometricNormal) * (input.backFace != 0 ? -1.0 : 1.0);
    float originDistance = max(parameters.voxelSize * 0.5, parameters.cellSize * 0.25);
    float3 origin = input.position + towardOrigin * originDistance;
    float endpointDistance = length(input.position - origin);
    float tolerance = VXGIProbeQueryEndpointTolerance(input.position, origin, endpointDistance, grid.cellSize);
    VXGISDFHit hit;
    bool found = VXGISDFTrace(origin, normalize(input.position - origin), 0.0, endpointDistance + tolerance,
        grid, nodes, offsets, references, triangles,
        materials, parameters.materialCount, materialSampler, parameters.materialMipLevel, hit);
    result.primaryTraceStatus = hit.traceStatus;
    result.primaryTriangleIndex = hit.triangleIndex;
    if (!found || hit.traceStatus != 1)
    {
        result.status = hit.traceStatus >= 2 ? 6u : 4u;
        results[id.x] = result; return;
    }
    if (hit.triangleIndex != input.triangleIndex || abs(hit.distance - endpointDistance) > tolerance
        || length(hit.position - input.position) > tolerance)
    {
        result.status = 5; results[id.x] = result; return;
    }
    if (triangles[hit.triangleIndex].materialId >= parameters.materialCount)
    {
        result.status = 7; results[id.x] = result; return;
    }
    VXGISurfaceMaterial material = VXGISampleSurfaceMaterial(hit, triangles, materials, materialSampler, parameters.materialMipLevel);
    if (material.valid == 0 || !all(isfinite(material.shadingNormal)) || dot(material.shadingNormal, material.shadingNormal) == 0.0)
    {
        result.status = 7; results[id.x] = result; return;
    }
    result.status = 1;
    result.query = VXGIProbeQuerySurface(hit, material.shadingNormal, parameters, lookup, centers, raw,
        nodes, offsets, references, triangles, materials, materialSampler);
    float3 latticePosition = hit.position / parameters.voxelSize - 0.5;
    float3 baseFloat = floor(latticePosition);
    int3 base = int3(baseFloat);
    float3 fraction = latticePosition - baseFloat;
    [unroll] for (uint cornerIndex = 0; cornerIndex < 8; ++cornerIndex)
    {
        int3 corner = int3(cornerIndex & 1u, (cornerIndex >> 1) & 1u, (cornerIndex >> 2) & 1u);
        float3 axisWeight = lerp(1.0 - fraction, fraction, float3(corner));
        if (axisWeight.x * axisWeight.y * axisWeight.z <= 0.0) continue;
        ++result.positiveCorners;
        uint probeIndex = VXGIProbeFindIndex(base + corner, lookup, parameters.probeCount);
        if (probeIndex == VXGI_PROBE_MISSING_INDEX) { ++result.missingCorners; continue; }
        VXGIProbeRecord source = raw[probeIndex];
        if (source.state == VXGI_PROBE_DISABLED) { ++result.disabledCorners; continue; }
        if (source.state != VXGI_PROBE_VALID) { ++result.unclassifiedCorners; continue; }
        if (source.updateCount == 0) { ++result.neverCapturedCorners; continue; }
        bool finite = true;
        [unroll] for (uint coefficient = 0; coefficient < 4; ++coefficient)
            finite = finite && all(isfinite(source.sh[coefficient]));
        if (!finite) { ++result.nonfiniteCorners; continue; }
        ++result.eligibleCorners;
        float3 facingNormal = hit.backFace != 0 ? -hit.geometricNormal : hit.geometricNormal;
        if (dot(facingNormal, centers[probeIndex] - hit.position) <= 0.0)
        { ++result.hemisphereRejectedCorners; continue; }
        bool failure;
        bool visible = VXGIProbeSurfaceVisibility(hit, centers[probeIndex], grid, parameters.triangleCount,
            nodes, offsets, references, triangles, materials, parameters.materialCount,
            materialSampler, parameters.materialMipLevel, failure);
        if (visible) ++result.acceptedCorners;
        else if (failure) ++result.visibilityFailures;
        else ++result.visibilityRejectedCorners;
    }
    results[id.x] = result;
}
