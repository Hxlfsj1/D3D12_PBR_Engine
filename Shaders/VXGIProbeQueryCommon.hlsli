#ifndef VXGI_PROBE_QUERY_COMMON_HLSLI
#define VXGI_PROBE_QUERY_COMMON_HLSLI

#include "VXGIProbeCommon.hlsli"
#include "VXGIProbeRadianceCommon.hlsli"

static const uint VXGI_PROBE_VISIBILITY_KNOWN = 0x80000000u;

// Exactly 24 bytes. Missing entries use UINT_MAX. Order: -X,+X,-Y,+Y,-Z,+Z.
struct VXGIProbeNeighborRecord
{
    uint indices[6];
};

// Five 16-byte rows, matching the 20-DWORD production root constants.
struct VXGIProbeQueryParameters
{
    float3 gridOrigin; float cellSize;
    uint3 cellDimensions; uint probeCount;
    float3 boundsMaximum; uint triangleCount;
    float voxelSize; uint updateStart; uint updateCount; uint resetMode;
    float neighborWeight; uint materialCount; float materialMipLevel; uint padding2;
};

// Exactly 48 bytes. supportWeight is the accepted, unnormalized trilinear sum.
// Zero radiance remains valid support; only geometry/state determine support.
struct VXGIProbeQueryResult
{
    float3 irradianceOverPi; float supportWeight;
    float3 unclampedIrradianceOverPi; uint negativeMask;
    uint candidateCount;
    uint acceptedCount;
    uint hasSupport;
    uint traceFailures;
};

VXGISDFGrid VXGIProbeQueryMakeGrid(VXGIProbeQueryParameters parameters)
{
    VXGISDFGrid grid;
    grid.origin = parameters.gridOrigin;
    grid.cellSize = parameters.cellSize;
    grid.cellDimensions = parameters.cellDimensions;
    grid.boundsMaximum = parameters.boundsMaximum;
    return grid;
}

bool VXGIProbeQueryGridValid(VXGISDFGrid grid)
{
    return all(isfinite(grid.origin)) && all(isfinite(grid.boundsMaximum))
        && isfinite(grid.cellSize) && grid.cellSize > 0.0
        && all(grid.cellDimensions > 0) && all(grid.boundsMaximum > grid.origin);
}

// This tolerance only verifies the identity of the first hit at the destination.
// It never creates a rayMin interval or exempts another triangle near an endpoint.
float VXGIProbeQueryEndpointTolerance(float3 surfacePosition, float3 center, float distance, float cellSize)
{
    float coordinateScale = max(distance,
        max(VXGIProbeMaximumCoordinate(surfacePosition), VXGIProbeMaximumCoordinate(center)));
    return max(max(1e-6, cellSize * 1e-5), 8.0 * 1.1920928955078125e-7 * coordinateScale);
}

// The caller supplies centers in canonical min-index -> max-index order. Both
// endpoints belong to the closed segment: a hit at either endpoint blocks it.
bool VXGIProbeCenterVisibility(float3 start, float3 end, VXGISDFGrid grid,
    StructuredBuffer<float> nodes, StructuredBuffer<uint> cellOffsets,
    StructuredBuffer<uint> triangleReferences, StructuredBuffer<VXGISDFTriangle> triangles,
    StructuredBuffer<VXGIMaterialData> materials, uint materialCount,
    SamplerState materialSampler, float materialMipLevel,
    out bool traceFailure)
{
    traceFailure = false;
    float3 delta = end - start;
    float distance = length(delta);
    if (!VXGIProbeQueryGridValid(grid) || !all(isfinite(start)) || !all(isfinite(end))
        || !isfinite(distance) || distance <= 0.0)
    {
        traceFailure = true;
        return false;
    }
    VXGISDFHit hit;
    bool found = VXGISDFTrace(start, delta / distance, 0.0, distance,
        grid, nodes, cellOffsets, triangleReferences, triangles,
        materials, materialCount, materialSampler, materialMipLevel, hit);
    traceFailure = hit.traceStatus >= 2;
    return !found && hit.traceStatus == 0;
}

// Trace the surface->probe segment in reverse. A clear result must terminate on
// the exact target triangle at the target point. This avoids a surface-origin
// bias, whole-triangle/object ignore rules, and normal offsets across thin walls.
// A miss cannot prove the required endpoint and therefore fails closed.
bool VXGIProbeSurfaceVisibility(VXGISDFHit surfaceHit, float3 center, VXGISDFGrid grid, uint triangleCount,
    StructuredBuffer<float> nodes, StructuredBuffer<uint> cellOffsets,
    StructuredBuffer<uint> triangleReferences, StructuredBuffer<VXGISDFTriangle> triangles,
    StructuredBuffer<VXGIMaterialData> materials, uint materialCount,
    SamplerState materialSampler, float materialMipLevel,
    out bool traceFailure)
{
    traceFailure = false;
    float3 towardCenter = center - surfaceHit.position;
    float distance = length(towardCenter);
    float maximumNormal = VXGIProbeMaximumCoordinate(surfaceHit.geometricNormal);
    if (!VXGIProbeQueryGridValid(grid) || surfaceHit.valid == 0 || surfaceHit.traceStatus != 1
        || surfaceHit.triangleIndex >= triangleCount || !all(isfinite(surfaceHit.position))
        || !all(isfinite(surfaceHit.geometricNormal)) || !all(isfinite(center))
        || maximumNormal == 0.0 || !isfinite(distance) || distance <= 0.0)
    {
        traceFailure = true;
        return false;
    }
    float3 geometricNormal = normalize(surfaceHit.geometricNormal / maximumNormal);
    if (surfaceHit.backFace != 0) geometricNormal = -geometricNormal;
    // Shading/normal-map normals are not used to choose the geometric side.
    if (dot(geometricNormal, towardCenter) <= 0.0) return false;
    float tolerance = VXGIProbeQueryEndpointTolerance(surfaceHit.position, center, distance, grid.cellSize);
    float maximumDistance = distance + tolerance;
    if (!isfinite(tolerance) || !isfinite(maximumDistance))
    {
        traceFailure = true;
        return false;
    }
    VXGISDFHit firstHit;
    bool found = VXGISDFTrace(center, -towardCenter / distance, 0.0, maximumDistance,
        grid, nodes, cellOffsets, triangleReferences, triangles,
        materials, materialCount, materialSampler, materialMipLevel, firstHit);
    if (!found || firstHit.traceStatus != 1)
    {
        traceFailure = true;
        return false;
    }
    // Other triangles are real blockers, even within an endpoint tolerance,
    // in the same object, or when they emit black radiance.
    if (firstHit.triangleIndex != surfaceHit.triangleIndex) return false;
#ifdef VXGI_MESH_SDF
    // A shared asset triangle may belong to several distinct world surfaces.
    if (firstHit.instanceIndex != surfaceHit.instanceIndex) return false;
#endif
    bool endpointVerified = abs(firstHit.distance - distance) <= tolerance
        && length(firstHit.position - surfaceHit.position) <= tolerance;
    traceFailure = !endpointVerified;
    return endpointVerified;
}

// One local filter of the immutable raw history. Its output is diagnostic data;
// surface queries never borrow this pre-mixed SH through a visible anchor.
VXGIProbeRecord VXGIProbeFilter(VXGIProbeRecord source, uint probeIndex, VXGIProbeQueryParameters parameters,
    StructuredBuffer<float3> centers, StructuredBuffer<VXGIProbeNeighborRecord> neighbors,
    StructuredBuffer<VXGIProbeRecord> rawRecords, StructuredBuffer<uint> visibility)
{
    if (probeIndex >= parameters.probeCount || !VXGIProbeQueryRecordValid(source))
        return (VXGIProbeRecord)0;
    VXGIProbeRecord result = source;
    float weight = isfinite(parameters.neighborWeight) ? saturate(parameters.neighborWeight) : 0.0;
    float totalWeight = 1.0;
    [unroll] for (uint coefficient = 0; coefficient < 4; ++coefficient)
        result.sh[coefficient] = float4(source.sh[coefficient].rgb, 0.0);
    uint mask = visibility[probeIndex];
    if ((mask & VXGI_PROBE_VISIBILITY_KNOWN) != 0 && weight > 0.0)
    {
        [unroll] for (uint neighborSlot = 0; neighborSlot < 6; ++neighborSlot)
        {
            uint neighborIndex = neighbors[probeIndex].indices[neighborSlot];
            if (neighborIndex >= parameters.probeCount || neighborIndex == probeIndex
                || (mask & (1u << neighborSlot)) == 0 || (mask & (1u << (neighborSlot + 8))) != 0) continue;
            VXGIProbeRecord neighbor = rawRecords[neighborIndex];
            if (!VXGIProbeQueryRecordValid(neighbor)) continue;
            [unroll] for (uint coefficient = 0; coefficient < 4; ++coefficient)
                result.sh[coefficient].rgb += neighbor.sh[coefficient].rgb * weight;
            totalWeight += weight;
        }
    }
    [unroll] for (uint coefficient = 0; coefficient < 4; ++coefficient)
        result.sh[coefficient].rgb /= totalWeight;
    return result;
}

// Eight centers of the world-origin probe lattice. Only individually visible
// raw contributors enter the estimate; missing support never borrows a distant
// or prefiltered anchor. Keep signed SH until after the caller evaluates it.
bool VXGIProbeGatherRawSH(VXGISDFHit surfaceHit,
    VXGIProbeQueryParameters parameters, StructuredBuffer<VXGIProbeCellEntry> lookup,
    StructuredBuffer<float3> centers, StructuredBuffer<VXGIProbeRecord> rawRecords,
    StructuredBuffer<float> nodes, StructuredBuffer<uint> cellOffsets,
    StructuredBuffer<uint> triangleReferences, StructuredBuffer<VXGISDFTriangle> triangles,
    StructuredBuffer<VXGIMaterialData> materials, SamplerState materialSampler,
    out VXGIProbeRecord estimate, out float supportWeight, out uint candidateCount,
    out uint acceptedCount, out uint traceFailures)
{
    estimate = (VXGIProbeRecord)0;
    supportWeight = 0.0;
    candidateCount = 0;
    acceptedCount = 0;
    traceFailures = 0;
    VXGISDFGrid grid = VXGIProbeQueryMakeGrid(parameters);
    if (!VXGIProbeQueryGridValid(grid) || !isfinite(parameters.voxelSize) || parameters.voxelSize <= 0.0
        || surfaceHit.valid == 0 || surfaceHit.traceStatus != 1 || surfaceHit.triangleIndex >= parameters.triangleCount
        || !all(isfinite(surfaceHit.position)))
    {
        traceFailures = 1;
        return false;
    }
    float3 latticePosition = surfaceHit.position / parameters.voxelSize - 0.5;
    float3 baseFloat = floor(latticePosition);
    // Guard float-to-int conversion and adding the corner's +1. The positive
    // bound is the largest float below INT_MAX, so the addition cannot overflow.
    if (!all(isfinite(latticePosition)) || any(baseFloat < -2147483648.0) || any(baseFloat > 2147483520.0))
    {
        traceFailures = 1;
        return false;
    }
    int3 base = int3(baseFloat);
    float3 fraction = latticePosition - baseFloat;
    [unroll] for (uint cornerIndex = 0; cornerIndex < 8; ++cornerIndex)
    {
        int3 corner = int3(cornerIndex & 1u, (cornerIndex >> 1) & 1u, (cornerIndex >> 2) & 1u);
        float3 axisWeight = lerp(1.0 - fraction, fraction, float3(corner));
        float weight = axisWeight.x * axisWeight.y * axisWeight.z;
        if (weight <= 0.0) continue;
        uint probeIndex = VXGIProbeFindIndex(base + corner, lookup, parameters.probeCount);
        if (probeIndex == VXGI_PROBE_MISSING_INDEX) continue;
        ++candidateCount;
        VXGIProbeRecord source = rawRecords[probeIndex];
        if (!VXGIProbeQueryRecordValid(source)) continue;
        bool traceFailure;
        if (!VXGIProbeSurfaceVisibility(surfaceHit, centers[probeIndex], grid, parameters.triangleCount,
            nodes, cellOffsets, triangleReferences, triangles,
            materials, parameters.materialCount, materialSampler, parameters.materialMipLevel, traceFailure))
        {
            traceFailures += traceFailure ? 1u : 0u;
            continue;
        }
        ++acceptedCount;
        supportWeight += weight;
        [unroll] for (uint coefficient = 0; coefficient < 4; ++coefficient)
            estimate.sh[coefficient].rgb += source.sh[coefficient].rgb * weight;
    }
    if (acceptedCount == 0 || supportWeight <= 0.0) return false;
    [unroll] for (uint coefficient = 0; coefficient < 4; ++coefficient)
        estimate.sh[coefficient].rgb /= supportWeight;
    return true;
}

// Retained for surface-irradiance diagnostics and consumers. Every SH uses the
// same [constant,X,Y,Z] world basis; diffuse convolution applies L1's 2/3 factor.
VXGIProbeQueryResult VXGIProbeQuerySurface(VXGISDFHit surfaceHit, float3 mappedNormal,
    VXGIProbeQueryParameters parameters, StructuredBuffer<VXGIProbeCellEntry> lookup,
    StructuredBuffer<float3> centers, StructuredBuffer<VXGIProbeRecord> rawRecords,
    StructuredBuffer<float> nodes, StructuredBuffer<uint> cellOffsets,
    StructuredBuffer<uint> triangleReferences, StructuredBuffer<VXGISDFTriangle> triangles,
    StructuredBuffer<VXGIMaterialData> materials, SamplerState materialSampler)
{
    VXGIProbeQueryResult result = (VXGIProbeQueryResult)0;
    float maximumNormal = VXGIProbeMaximumCoordinate(mappedNormal);
    if (!all(isfinite(mappedNormal)) || maximumNormal == 0.0)
    {
        result.traceFailures = 1;
        return result;
    }
    VXGIProbeRecord estimate;
    if (!VXGIProbeGatherRawSH(surfaceHit, parameters, lookup, centers, rawRecords,
        nodes, cellOffsets, triangleReferences, triangles, materials, materialSampler,
        estimate, result.supportWeight, result.candidateCount, result.acceptedCount, result.traceFailures))
        return result;
    float3 normal = normalize(mappedNormal / maximumNormal);
    float4 basis = VXGIProbeSHBasis(normal);
    result.unclampedIrradianceOverPi = estimate.sh[0].rgb * basis.x
        + (2.0 / 3.0) * (estimate.sh[1].rgb * basis.y
            + estimate.sh[2].rgb * basis.z + estimate.sh[3].rgb * basis.w);
    result.negativeMask = (result.unclampedIrradianceOverPi.r < 0.0 ? 1u : 0u)
        | (result.unclampedIrradianceOverPi.g < 0.0 ? 2u : 0u)
        | (result.unclampedIrradianceOverPi.b < 0.0 ? 4u : 0u);
    result.irradianceOverPi = max(result.unclampedIrradianceOverPi, 0.0);
    result.hasSupport = 1;
    return result;
}

#endif
