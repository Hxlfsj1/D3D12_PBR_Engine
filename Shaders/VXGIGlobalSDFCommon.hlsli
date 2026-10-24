#ifndef VXGI_GLOBAL_SDF_COMMON_HLSLI
#define VXGI_GLOBAL_SDF_COMMON_HLSLI

// This is an unsigned distance lower bound assembled from the existing mesh
// fields. It supplies geometry queries only; no radiance or material is stored.
static const uint VXGI_GLOBAL_SDF_MAX_CLIPMAPS = 3;
static const uint VXGI_GLOBAL_SDF_MISS = 0;
static const uint VXGI_GLOBAL_SDF_HIT = 1;
static const uint VXGI_GLOBAL_SDF_INVALID = 2;
static const uint VXGI_GLOBAL_SDF_EXHAUSTED = 3;
static const float VXGI_GLOBAL_SDF_INTERPOLATION_ERROR = 0.866025404;

struct VXGIGlobalSDFClipmap
{
    float3 origin; float cellSize;
    float3 boundsMaximum; uint nodeOffsetZ;
    uint3 scrollOffset; float distanceLimit;
};

// Shared 176-byte CPU/HLSL constant-buffer contract. Resolutions count NODES.
// Layers may have different sizes and are packed consecutively along texture Z;
// resolution is the maximum layer resolution and the atlas width/height.
struct VXGIGlobalSDFConstants
{
    uint resolution; uint clipmapCount; uint assetCount; uint nodeCount;
    uint instanceCount; uint3 clipmapResolutions;
    VXGIGlobalSDFClipmap clipmaps[VXGI_GLOBAL_SDF_MAX_CLIPMAPS];
};

// UINT_MAX selects the full, Z-stacked atlas. Otherwise minimum/size describe
// a logical box in one clipmap; rolling physical storage is applied at writes.
struct VXGIGlobalSDFUpdateRegion
{
    uint3 minimum; uint clipmapIndex;
    uint3 size; uint padding;
};

struct VXGIGlobalSDFSample
{
    bool valid;
    float distance;
    float safeDistance;
    float cellSize;
    uint clipmapIndex;
};

struct VXGIGlobalSDFTraceResult
{
    uint status;
    float3 position;
    float distance;
    float fieldDistance;
    uint clipmapIndex;
    uint sampleCount;
};

bool VXGIGlobalSDFValidConstants(VXGIGlobalSDFConstants constants)
{
    if (constants.resolution < 3 || constants.resolution > 257 || constants.resolution % 2 == 0 ||
        constants.clipmapCount == 0 || constants.clipmapCount > VXGI_GLOBAL_SDF_MAX_CLIPMAPS)
        return false;
    uint offsetZ = 0;
    uint maximumResolution = 0;
    [unroll] for (uint index = 0; index < VXGI_GLOBAL_SDF_MAX_CLIPMAPS; ++index)
    {
        if (index >= constants.clipmapCount) break;
        VXGIGlobalSDFClipmap clipmap = constants.clipmaps[index];
        uint resolution = constants.clipmapResolutions[index];
        if (resolution < 3 || resolution > constants.resolution || resolution % 2 == 0 ||
            !all(isfinite(clipmap.origin)) || !all(isfinite(clipmap.boundsMaximum)) ||
            !isfinite(clipmap.cellSize) || clipmap.cellSize <= 0.0 ||
            !isfinite(clipmap.distanceLimit) || clipmap.distanceLimit <= 0.0 ||
            any(clipmap.scrollOffset >= resolution) ||
            any(clipmap.boundsMaximum <= clipmap.origin) ||
            clipmap.nodeOffsetZ != offsetZ)
            return false;
        float3 expectedMaximum = clipmap.origin + float(resolution - 1) * clipmap.cellSize;
        float3 tolerance = max(clipmap.cellSize * 1e-4, abs(expectedMaximum) * 2e-6);
        if (!all(isfinite(expectedMaximum)) ||
            any(abs(clipmap.boundsMaximum - expectedMaximum) > tolerance))
            return false;
        // The last layer is the full coverage envelope used for ray clipping.
        if (index != 0 && (clipmap.cellSize <= constants.clipmaps[index - 1].cellSize ||
            any(clipmap.origin > constants.clipmaps[index - 1].origin) ||
            any(clipmap.boundsMaximum < constants.clipmaps[index - 1].boundsMaximum)))
            return false;
        offsetZ += resolution;
        maximumResolution = max(maximumResolution, resolution);
    }
    return maximumResolution == constants.resolution && offsetZ <= 2048;
}

bool VXGIGlobalSDFValidTexture(Texture3D<float> field, VXGIGlobalSDFConstants constants)
{
    if (!VXGIGlobalSDFValidConstants(constants)) return false;
    uint width, height, depth;
    field.GetDimensions(width, height, depth);
    uint outer = constants.clipmapCount - 1;
    return width == constants.resolution && height == constants.resolution &&
        depth == constants.clipmaps[outer].nodeOffsetZ + constants.clipmapResolutions[outer];
}

uint VXGIGlobalSDFSelectClipmap(VXGIGlobalSDFConstants constants, float3 position)
{
    [unroll] for (uint index = 0; index < VXGI_GLOBAL_SDF_MAX_CLIPMAPS; ++index)
    {
        if (index >= constants.clipmapCount) break;
        VXGIGlobalSDFClipmap clipmap = constants.clipmaps[index];
        if (all(position >= clipmap.origin) && all(position <= clipmap.boundsMaximum)) return index;
    }
    return 0xffffffffu;
}

uint3 VXGIGlobalSDFPhysicalNode(VXGIGlobalSDFConstants constants,
    uint clipmapIndex, uint3 logicalNode)
{
    VXGIGlobalSDFClipmap clipmap = constants.clipmaps[clipmapIndex];
    uint3 physical = (logicalNode + clipmap.scrollOffset) % constants.clipmapResolutions[clipmapIndex];
    physical.z += clipmap.nodeOffsetZ;
    return physical;
}

VXGIGlobalSDFSample VXGIGlobalSDFSampleClipmapUnchecked(Texture3D<float> field,
    VXGIGlobalSDFConstants constants, uint clipmapIndex, float3 position)
{
    VXGIGlobalSDFSample result = (VXGIGlobalSDFSample)0;
    result.clipmapIndex = clipmapIndex;
    VXGIGlobalSDFClipmap clipmap = constants.clipmaps[clipmapIndex];
    uint resolution = constants.clipmapResolutions[clipmapIndex];
    result.cellSize = clipmap.cellSize;
    float3 coordinate = clamp((position - clipmap.origin) / clipmap.cellSize,
        0.0, float(resolution - 1));
    if (!all(isfinite(coordinate))) return result;
    uint3 cell = min(uint3(floor(coordinate)), resolution - 2);
    float3 fraction = saturate(coordinate - float3(cell));
    // Wrap every corner independently. Wrapping only the base node would read
    // a different row/layer when a physical cell straddles the rolling seam.
    float d000 = field.Load(int4(VXGIGlobalSDFPhysicalNode(constants, clipmapIndex, cell), 0));
    float d100 = field.Load(int4(VXGIGlobalSDFPhysicalNode(constants, clipmapIndex, cell + uint3(1, 0, 0)), 0));
    float d010 = field.Load(int4(VXGIGlobalSDFPhysicalNode(constants, clipmapIndex, cell + uint3(0, 1, 0)), 0));
    float d110 = field.Load(int4(VXGIGlobalSDFPhysicalNode(constants, clipmapIndex, cell + uint3(1, 1, 0)), 0));
    float d001 = field.Load(int4(VXGIGlobalSDFPhysicalNode(constants, clipmapIndex, cell + uint3(0, 0, 1)), 0));
    float d101 = field.Load(int4(VXGIGlobalSDFPhysicalNode(constants, clipmapIndex, cell + uint3(1, 0, 1)), 0));
    float d011 = field.Load(int4(VXGIGlobalSDFPhysicalNode(constants, clipmapIndex, cell + uint3(0, 1, 1)), 0));
    float d111 = field.Load(int4(VXGIGlobalSDFPhysicalNode(constants, clipmapIndex, cell + uint3(1, 1, 1)), 0));
    if (!all(isfinite(float4(d000, d100, d010, d110))) ||
        !all(isfinite(float4(d001, d101, d011, d111))) ||
        any(float4(d000, d100, d010, d110) < 0.0) ||
        any(float4(d001, d101, d011, d111) < 0.0)) return result;
    result.distance = lerp(lerp(lerp(d000, d100, fraction.x), lerp(d010, d110, fraction.x), fraction.y),
        lerp(lerp(d001, d101, fraction.x), lerp(d011, d111, fraction.x), fraction.y), fraction.z);
    // Each composed node is <= the exact geometry distance at that node.
    // Interpolation can overestimate by at most sqrt(3)/2 cells, even for an
    // unsigned field around a thin/open surface. Subtract it before stepping.
    float roundoff = max(clipmap.cellSize * 1e-4, abs(result.distance) * 2e-6);
    result.safeDistance = max(0.0, result.distance -
        VXGI_GLOBAL_SDF_INTERPOLATION_ERROR * clipmap.cellSize - roundoff);
    result.valid = isfinite(result.distance) && isfinite(result.safeDistance);
    return result;
}

VXGIGlobalSDFSample VXGIGlobalSDFSampleDistance(Texture3D<float> field,
    VXGIGlobalSDFConstants constants, float3 position)
{
    VXGIGlobalSDFSample result = (VXGIGlobalSDFSample)0;
    result.clipmapIndex = 0xffffffffu;
    if (!all(isfinite(position)) || !VXGIGlobalSDFValidTexture(field, constants)) return result;
    uint index = VXGIGlobalSDFSelectClipmap(constants, position);
    if (index == 0xffffffffu) return result;
    return VXGIGlobalSDFSampleClipmapUnchecked(field, constants, index, position);
}

bool VXGIGlobalSDFRayBounds(float3 origin, float3 direction,
    VXGIGlobalSDFClipmap clipmap, float rayMin, float rayMax, out float entry, out float exit)
{
    entry = rayMin;
    exit = rayMax;
    [unroll] for (uint axis = 0; axis < 3; ++axis)
    {
        if (direction[axis] == 0.0)
        {
            if (origin[axis] < clipmap.origin[axis] || origin[axis] > clipmap.boundsMaximum[axis])
                return false;
        }
        else
        {
            float a = (clipmap.origin[axis] - origin[axis]) / direction[axis];
            float b = (clipmap.boundsMaximum[axis] - origin[axis]) / direction[axis];
            entry = max(entry, min(a, b));
            exit = min(exit, max(a, b));
        }
    }
    return entry <= exit;
}

VXGIGlobalSDFTraceResult VXGIGlobalSDFTrace(Texture3D<float> field,
    VXGIGlobalSDFConstants constants, float3 origin, float3 direction,
    float rayMin, float rayMax, uint maxSteps)
{
    VXGIGlobalSDFTraceResult result = (VXGIGlobalSDFTraceResult)0;
    result.status = VXGI_GLOBAL_SDF_INVALID;
    result.clipmapIndex = 0xffffffffu;
    if (!all(isfinite(origin)) || !all(isfinite(direction)) ||
        !isfinite(rayMin) || !isfinite(rayMax) || rayMin < 0.0 || rayMax < rayMin ||
        maxSteps == 0 || !VXGIGlobalSDFValidTexture(field, constants)) return result;
    float largest = max(abs(direction.x), max(abs(direction.y), abs(direction.z)));
    if (largest == 0.0) return result;
    float3 scaled = direction / largest;
    float3 unitDirection = scaled / sqrt(dot(scaled, scaled));
    if (!all(isfinite(unitDirection))) return result;

    float entry[VXGI_GLOBAL_SDF_MAX_CLIPMAPS];
    float exit[VXGI_GLOBAL_SDF_MAX_CLIPMAPS];
    uint intersects[VXGI_GLOBAL_SDF_MAX_CLIPMAPS];
    [unroll] for (uint index = 0; index < VXGI_GLOBAL_SDF_MAX_CLIPMAPS; ++index)
    {
        entry[index] = rayMin;
        exit[index] = rayMax;
        intersects[index] = 0;
        if (index < constants.clipmapCount)
            intersects[index] = VXGIGlobalSDFRayBounds(origin, unitDirection,
                constants.clipmaps[index], rayMin, rayMax, entry[index], exit[index]) ? 1u : 0u;
    }
    uint outer = constants.clipmapCount - 1;
    result.status = VXGI_GLOBAL_SDF_MISS;
    if (intersects[outer] == 0) return result;
    float current = entry[outer];
    float end = exit[outer];
    [loop] for (uint step = 0; step < maxSteps; ++step)
    {
        // Select with the slab interval instead of a reconstructed position:
        // floating-point rounding at an entering face must not keep the coarse
        // layer active after the ray has reached a finer layer.
        uint clipmapIndex = outer;
        float nextBoundary = end;
        [unroll] for (uint index = 0; index < VXGI_GLOBAL_SDF_MAX_CLIPMAPS; ++index)
        {
            if (index >= constants.clipmapCount) break;
            if (intersects[index] == 0) continue;
            if (current >= entry[index] && current <= exit[index])
                clipmapIndex = min(clipmapIndex, index);
            if (entry[index] > current) nextBoundary = min(nextBoundary, entry[index]);
            if (exit[index] > current) nextBoundary = min(nextBoundary, exit[index]);
        }
        float3 position = origin + unitDirection * current;
        if (!all(isfinite(position)))
        {
            result.status = VXGI_GLOBAL_SDF_INVALID;
            return result;
        }
        VXGIGlobalSDFClipmap clipmap = constants.clipmaps[clipmapIndex];
        VXGIGlobalSDFSample sample = VXGIGlobalSDFSampleClipmapUnchecked(field, constants,
            clipmapIndex, clamp(position, clipmap.origin, clipmap.boundsMaximum));
        ++result.sampleCount;
        result.position = position;
        result.distance = current;
        result.fieldDistance = sample.distance;
        result.clipmapIndex = clipmapIndex;
        if (!sample.valid)
        {
            result.status = VXGI_GLOBAL_SDF_INVALID;
            return result;
        }
        if (sample.distance <= 0.9 * clipmap.cellSize)
        {
            result.status = VXGI_GLOBAL_SDF_HIT;
            return result;
        }
        if (current >= end) return result;
        // Always visit clipmap entry/exit faces. In particular a large coarse
        // step cannot cross an entire fine clipmap without querying its field.
        float next = min(current + 0.9 * sample.safeDistance, nextBoundary);
        if (!(next > current))
        {
            result.status = VXGI_GLOBAL_SDF_EXHAUSTED;
            return result;
        }
        current = next;
    }
    result.status = VXGI_GLOBAL_SDF_EXHAUSTED;
    return result;
}

#endif
