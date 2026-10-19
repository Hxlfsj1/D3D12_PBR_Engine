#ifndef VXGI_SDF_THRESHOLD_COMMON_HLSLI
#define VXGI_SDF_THRESHOLD_COMMON_HLSLI

// Approximate unsigned-field queries only. These functions read scalar node
// distances, never triangles, materials, or the exact-tracing acceleration.
// The caller supplies the original finite ray interval for one model candidate.
struct VXGIThresholdGrid
{
    float3 origin; float cellSize;
    uint3 cellDimensions; uint nodeOffset;
    float3 boundsMaximum; uint padding;
};

struct VXGIThresholdParameters
{
    float hitThresholdCells;
    float stepScale;
    uint maxSteps;
    uint padding;
};

static const uint VXGI_THRESHOLD_MISS = 0;
static const uint VXGI_THRESHOLD_HIT = 1;
static const uint VXGI_THRESHOLD_INVALID = 2;
static const uint VXGI_THRESHOLD_EXHAUSTED = 3;
static const uint VXGI_THRESHOLD_STALLED = 4;
static const float VXGI_THRESHOLD_MAX_INTERPOLATION_ERROR_CELLS = 0.866025404;

struct VXGIThresholdSample
{
    bool valid;
    float distance;
    float uncertainty;
    float safeDistance;
};

struct VXGIThresholdResult
{
    uint status;
    float3 position;
    float distance;
    float fieldDistance;
    uint sampleCount;
};

bool VXGIThresholdValidGrid(VXGIThresholdGrid grid, uint nodeCount)
{
    if (!all(isfinite(grid.origin)) || !all(isfinite(grid.boundsMaximum)) ||
        !isfinite(grid.cellSize) || grid.cellSize <= 0.0 ||
        any(grid.boundsMaximum <= grid.origin) ||
        !all(isfinite(grid.origin + float3(grid.cellDimensions) * grid.cellSize)) ||
        any(grid.cellDimensions == 0) ||
        any(grid.cellDimensions == 0xffffffffu) || grid.nodeOffset > nodeCount)
        return false;
    uint available = nodeCount - grid.nodeOffset;
    uint3 dimensions = grid.cellDimensions + 1;
    // Divide before multiplying so malformed descriptors cannot wrap indexing.
    if (dimensions.x > available) return false;
    uint count = dimensions.x;
    if (dimensions.y > available / count) return false;
    count *= dimensions.y;
    if (dimensions.z > available / count) return false;
    return true;
}

uint VXGIThresholdNodeIndex(uint3 node, uint3 dimensions, uint offset)
{
    return offset + node.x + dimensions.x * (node.y + dimensions.y * node.z);
}

// Grid, containment, and total buffer size have already been validated.
VXGIThresholdSample VXGIThresholdSampleUnchecked(StructuredBuffer<float> nodes,
    VXGIThresholdGrid grid, float3 position)
{
    VXGIThresholdSample result = (VXGIThresholdSample)0;
    float3 coordinate = (position - grid.origin) / grid.cellSize;
    if (!all(isfinite(coordinate))) return result;
    coordinate = clamp(coordinate, 0.0, float3(grid.cellDimensions));
    uint3 cell = min(uint3(floor(coordinate)), grid.cellDimensions - 1);
    float3 f = clamp(coordinate - float3(cell), 0.0, 1.0);
    uint3 dimensions = grid.cellDimensions + 1;
    float d000 = nodes[VXGIThresholdNodeIndex(cell, dimensions, grid.nodeOffset)];
    float d100 = nodes[VXGIThresholdNodeIndex(cell + uint3(1, 0, 0), dimensions, grid.nodeOffset)];
    float d010 = nodes[VXGIThresholdNodeIndex(cell + uint3(0, 1, 0), dimensions, grid.nodeOffset)];
    float d110 = nodes[VXGIThresholdNodeIndex(cell + uint3(1, 1, 0), dimensions, grid.nodeOffset)];
    float d001 = nodes[VXGIThresholdNodeIndex(cell + uint3(0, 0, 1), dimensions, grid.nodeOffset)];
    float d101 = nodes[VXGIThresholdNodeIndex(cell + uint3(1, 0, 1), dimensions, grid.nodeOffset)];
    float d011 = nodes[VXGIThresholdNodeIndex(cell + uint3(0, 1, 1), dimensions, grid.nodeOffset)];
    float d111 = nodes[VXGIThresholdNodeIndex(cell + uint3(1, 1, 1), dimensions, grid.nodeOffset)];
    if (!all(isfinite(float4(d000, d100, d010, d110))) ||
        !all(isfinite(float4(d001, d101, d011, d111))) ||
        any(float4(d000, d100, d010, d110) < 0.0) ||
        any(float4(d001, d101, d011, d111) < 0.0))
        return result;
    result.distance = lerp(lerp(lerp(d000, d100, f.x), lerp(d010, d110, f.x), f.y),
        lerp(lerp(d001, d101, f.x), lerp(d011, d111, f.x), f.y), f.z);
    // For a 1-Lipschitz distance field, interpolation error is at most the
    // weighted corner offset; its RMS bound is h*sqrt(sum(f*(1-f))). At a cell
    // center this is sqrt(3)/2*h, including unsigned thin/open surfaces.
    result.uncertainty = grid.cellSize * sqrt(dot(f, 1.0 - f));
    float roundoff = max(grid.cellSize * 1e-4, abs(result.distance) * 2e-6);
    result.safeDistance = max(0.0, result.distance - result.uncertainty - roundoff);
    result.valid = isfinite(result.distance) && isfinite(result.uncertainty) &&
        isfinite(result.safeDistance);
    return result;
}

VXGIThresholdSample VXGIThresholdSampleDistance(StructuredBuffer<float> nodes,
    uint nodeCount, VXGIThresholdGrid grid, float3 position)
{
    if (!VXGIThresholdValidGrid(grid, nodeCount) || !all(isfinite(position)) ||
        any(position < grid.origin) || any(position > grid.boundsMaximum))
        return (VXGIThresholdSample)0;
    return VXGIThresholdSampleUnchecked(nodes, grid, position);
}

bool VXGIThresholdNormalize(float3 direction, out float3 unitDirection)
{
    unitDirection = 0.0;
    if (!all(isfinite(direction))) return false;
    float scale = max(abs(direction.x), max(abs(direction.y), abs(direction.z)));
    if (scale == 0.0) return false;
    float3 scaled = direction / scale;
    unitDirection = scaled / sqrt(dot(scaled, scaled));
    return all(isfinite(unitDirection));
}

bool VXGIThresholdRayBounds(float3 origin, float3 unitDirection,
    VXGIThresholdGrid grid, float rayMin, float rayMax, out float entry, out float exit)
{
    entry = rayMin;
    exit = rayMax;
    [unroll] for (uint axis = 0; axis < 3; ++axis)
    {
        if (unitDirection[axis] == 0.0)
        {
            if (origin[axis] < grid.origin[axis] || origin[axis] > grid.boundsMaximum[axis])
                return false;
        }
        else
        {
            float a = (grid.origin[axis] - origin[axis]) / unitDirection[axis];
            float b = (grid.boundsMaximum[axis] - origin[axis]) / unitDirection[axis];
            entry = max(entry, min(a, b));
            exit = min(exit, max(a, b));
        }
    }
    return entry <= exit;
}

VXGIThresholdResult VXGIThresholdTraceLocal(StructuredBuffer<float> nodes,
    uint nodeCount, VXGIThresholdGrid grid, float3 rayOrigin, float3 direction,
    float rayMin, float rayMax, VXGIThresholdParameters parameters)
{
    VXGIThresholdResult result = (VXGIThresholdResult)0;
    result.status = VXGI_THRESHOLD_INVALID;
    float3 unitDirection;
    if (!VXGIThresholdValidGrid(grid, nodeCount) || !all(isfinite(rayOrigin)) ||
        !isfinite(rayMin) || !isfinite(rayMax) || rayMin < 0.0 || rayMax < rayMin ||
        !isfinite(parameters.hitThresholdCells) ||
        parameters.hitThresholdCells <= VXGI_THRESHOLD_MAX_INTERPOLATION_ERROR_CELLS ||
        !isfinite(parameters.stepScale) || parameters.stepScale <= 0.0 ||
        parameters.stepScale > 1.0 || parameters.maxSteps == 0 ||
        !VXGIThresholdNormalize(direction, unitDirection))
        return result;
    float threshold = parameters.hitThresholdCells * grid.cellSize;
    if (!isfinite(threshold)) return result;
    float current, exit;
    if (!VXGIThresholdRayBounds(rayOrigin, unitDirection, grid, rayMin, rayMax, current, exit))
    {
        result.status = VXGI_THRESHOLD_MISS;
        return result;
    }
    [loop] for (uint stepIndex = 0; stepIndex < parameters.maxSteps; ++stepIndex)
    {
        float3 rayPosition = rayOrigin + unitDirection * current;
        if (!all(isfinite(rayPosition))) return result;
        // Slab clipping already placed current in the finite domain. Correct
        // only reconstruction roundoff at its border; never extend ray T.
        float3 position = clamp(rayPosition, grid.origin, grid.boundsMaximum);
        VXGIThresholdSample sample = VXGIThresholdSampleUnchecked(nodes, grid, position);
        ++result.sampleCount;
        result.position = rayPosition;
        result.distance = current;
        result.fieldDistance = sample.distance;
        if (!sample.valid) return result;
        if (sample.distance <= threshold)
        {
            result.status = VXGI_THRESHOLD_HIT;
            return result;
        }
        if (current >= exit)
        {
            result.status = VXGI_THRESHOLD_MISS;
            return result;
        }
        float next = min(current + parameters.stepScale * sample.safeDistance, exit);
        if (!(next > current))
        {
            result.status = VXGI_THRESHOLD_STALLED;
            return result;
        }
        current = next;
    }
    result.status = VXGI_THRESHOLD_EXHAUSTED;
    return result;
}

#endif
