#ifndef VXGI_PROBE_RADIANCE_COMMON_HLSLI
#define VXGI_PROBE_RADIANCE_COMMON_HLSLI

#include "VXGIProbeData.hlsli"

static const uint VXGI_PROBE_MISSING_INDEX = 0xffffffffu;

// World-origin cell coordinates, sorted lexicographically by X, Y, then Z.
// One entry per allocated probe; exactly 16 bytes.
struct VXGIProbeCellEntry
{
    int3 coordinate;
    uint probeIndex;
};

// Directional incoming radiance in the same world basis used when probes are
// accumulated. This is not diffuse irradiance and carries no material response.
struct VXGIProbeRadianceQueryResult
{
    float3 radiance; float supportWeight;
    float3 unclampedRadiance; uint negativeMask;
    uint candidateCount;
    uint acceptedCount;
    uint hasSupport;
    uint traceFailures;
};

bool VXGIProbeQueryRecordValid(VXGIProbeRecord record)
{
    if (record.state != VXGI_PROBE_VALID || record.updateCount == 0) return false;
    [unroll] for (uint coefficient = 0; coefficient < 4; ++coefficient)
        if (!all(isfinite(record.sh[coefficient]))) return false;
    return true;
}

bool VXGIProbeCoordinateLess(int3 a, int3 b)
{
    return a.x < b.x || (a.x == b.x && (a.y < b.y || (a.y == b.y && a.z < b.z)));
}

uint VXGIProbeFindIndex(int3 coordinate, StructuredBuffer<VXGIProbeCellEntry> lookup, uint probeCount)
{
    uint first = 0;
    uint last = probeCount;
    [loop] while (first < last)
    {
        uint middle = first + (last - first) / 2;
        VXGIProbeCellEntry entry = lookup[middle];
        if (all(entry.coordinate == coordinate))
            return entry.probeIndex < probeCount ? entry.probeIndex : VXGI_PROBE_MISSING_INDEX;
        if (VXGIProbeCoordinateLess(entry.coordinate, coordinate)) first = middle + 1;
        else last = middle;
    }
    return VXGI_PROBE_MISSING_INDEX;
}

float VXGIProbeMaximumCoordinate(float3 value)
{
    return max(abs(value.x), max(abs(value.y), abs(value.z)));
}

// Read only the cache at the world hit in the original GI ray's direction.
// Geometry has already been intersected by the caller; no normals, material,
// geometry resources or secondary visibility rays enter this lookup.
VXGIProbeRadianceQueryResult VXGIProbeQueryRadiance(float3 worldPosition, float3 worldRayDirection,
    float voxelSize, uint probeCount, StructuredBuffer<VXGIProbeCellEntry> lookup,
    StructuredBuffer<VXGIProbeRecord> rawRecords)
{
    VXGIProbeRadianceQueryResult result = (VXGIProbeRadianceQueryResult)0;
    float maximumDirection = VXGIProbeMaximumCoordinate(worldRayDirection);
    if (!all(isfinite(worldPosition)) || !all(isfinite(worldRayDirection)) || maximumDirection == 0.0
        || !isfinite(voxelSize) || voxelSize <= 0.0)
    {
        // Legacy result field: invalid query inputs, not a visibility trace.
        result.traceFailures = 1;
        return result;
    }
    float3 latticePosition = worldPosition / voxelSize - 0.5;
    float3 baseFloat = floor(latticePosition);
    if (!all(isfinite(latticePosition)) || any(baseFloat < -2147483648.0) || any(baseFloat > 2147483520.0))
    {
        result.traceFailures = 1;
        return result;
    }
    int3 base = int3(baseFloat);
    float3 fraction = latticePosition - baseFloat;
    VXGIProbeRecord estimate = (VXGIProbeRecord)0;
    [unroll] for (uint cornerIndex = 0; cornerIndex < 8; ++cornerIndex)
    {
        int3 corner = int3(cornerIndex & 1u, (cornerIndex >> 1) & 1u, (cornerIndex >> 2) & 1u);
        float3 axisWeight = lerp(1.0 - fraction, fraction, float3(corner));
        float weight = axisWeight.x * axisWeight.y * axisWeight.z;
        if (weight <= 0.0) continue;
        uint probeIndex = VXGIProbeFindIndex(base + corner, lookup, probeCount);
        if (probeIndex == VXGI_PROBE_MISSING_INDEX) continue;
        ++result.candidateCount;
        VXGIProbeRecord source = rawRecords[probeIndex];
        if (!VXGIProbeQueryRecordValid(source)) continue;
        ++result.acceptedCount;
        result.supportWeight += weight;
        [unroll] for (uint coefficient = 0; coefficient < 4; ++coefficient)
            estimate.sh[coefficient].rgb += source.sh[coefficient].rgb * weight;
    }
    if (result.acceptedCount == 0 || result.supportWeight <= 0.0) return result;
    [unroll] for (uint coefficient = 0; coefficient < 4; ++coefficient)
        estimate.sh[coefficient].rgb /= result.supportWeight;
    float3 direction = normalize(worldRayDirection / maximumDirection);
    float4 basis = VXGIProbeSHBasis(direction);
    result.unclampedRadiance = estimate.sh[0].rgb * basis.x
        + estimate.sh[1].rgb * basis.y + estimate.sh[2].rgb * basis.z + estimate.sh[3].rgb * basis.w;
    result.negativeMask = (result.unclampedRadiance.r < 0.0 ? 1u : 0u)
        | (result.unclampedRadiance.g < 0.0 ? 2u : 0u)
        | (result.unclampedRadiance.b < 0.0 ? 4u : 0u);
    result.radiance = max(result.unclampedRadiance, 0.0);
    result.hasSupport = 1;
    return result;
}

#endif
