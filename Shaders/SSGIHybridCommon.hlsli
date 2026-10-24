#ifndef SSGI_HYBRID_COMMON_HLSLI
#define SSGI_HYBRID_COMMON_HLSLI

#ifdef VXGI_SSGI_THRESHOLD
#include "VXGIProbeRadianceCommon.hlsli"
#include "VXGIMeshSDFThresholdCommon.hlsli"
#else
#include "VXGIProbeQueryCommon.hlsli"
#endif
#ifdef VXGI_SSGI_GLOBAL
#include "VXGIGlobalSDFCommon.hlsli"
#endif

static const uint SSGI_SCREEN_MISS = 0;
static const uint SSGI_SCREEN_HIT = 1;
static const uint SSGI_SCREEN_OUT_OF_SCREEN = 2;
static const uint SSGI_SCREEN_RADIUS_LIMIT = 3;
static const uint SSGI_SCREEN_ITERATION_LIMIT = 4;
static const uint SSGI_SCREEN_INVALID = 5;
static const uint SSGI_BACKEND_NONE = 0;
static const uint SSGI_BACKEND_SCREEN = 1;
static const uint SSGI_BACKEND_WORLD = 2;
static const uint SSGI_BACKEND_GLOBAL = 3;
static const uint SSGI_ANCHOR_UNUSED = 0;
static const uint SSGI_ANCHOR_VERIFIED = 1;
static const uint SSGI_ANCHOR_MISS = 2;
static const uint SSGI_ANCHOR_INVALID = 3;
static const uint SSGI_ANCHOR_EXHAUSTED = 4;
static const uint SSGI_ANCHOR_MISMATCH = 5;

#ifdef VXGI_SSGI_THRESHOLD
static const float SSGI_WORLD_HIT_THRESHOLD_CELLS = 0.9;
static const float SSGI_WORLD_NORMAL_BIAS_SCALE = 1.1;
#endif

// Legacy validation keeps six rows. Production appends explicit mesh/BVH
// counts as a seventh row: 112B here, 624B for the complete trace constants.
struct SSGIHybridParameters
{
    float3 gridOrigin; float cellSize;
    uint3 cellDimensions; uint triangleCount;
    float3 boundsMaximum; uint materialCount;
    float3 reservedLighting0; float materialMipLevel;
    float3 reservedLighting1; float reservedLighting2;
    float reservedWorldRange; float voxelSize; uint probeCount; uint worldMode;
#ifdef VXGI_SSGI_THRESHOLD
    uint nodeCount; uint assetCount; uint instanceCount; uint bvhNodeCount;
#endif
};

#ifdef VXGI_SSGI_GLOBAL
// 192B appended after the existing 784B trace contract. The worldMode SRV
// alias and every history/checkerboard field keep their existing offsets.
struct SSGIGlobalParameters
{
    uint enabled; uint textureIndex; float maxTraceDistance; uint maxSteps;
    VXGIGlobalSDFConstants constants;
};
#endif

// 48B. viewPoint is the fine hit pixel-center's depth surface; rayPoint is
// the candidate on the traversed screen segment. Confidence is binary geometry
// acceptance, never radiance, AO, or an averaging/normalization weight.
struct SSGIScreenRayResult
{
    float3 viewPoint; float distance;
    uint2 hitPixel; uint status; float confidence;
    float3 rayPoint; float padding;
};

// 64B diagnostic result. Hit is a geometric termination, independent of RGB
// and source/query validity. Failed source evaluation must not trace behind it.
struct SSGIHybridRayResult
{
    float3 radiance; uint hit;
    float distance; uint status; uint backend; uint worldTraceStatus;
    float confidence; uint anchorStatus; uint triangleIndex; uint probeHasSupport;
    float probeSupportWeight; uint probeAcceptedCount; uint probeTraceFailures; uint sourceValid;
};

#ifndef VXGI_SSGI_THRESHOLD
VXGISDFGrid SSGIHybridMakeGrid(SSGIHybridParameters parameters)
{
    VXGISDFGrid grid;
    grid.origin = parameters.gridOrigin;
    grid.cellSize = parameters.cellSize;
    grid.cellDimensions = parameters.cellDimensions;
    grid.boundsMaximum = parameters.boundsMaximum;
    return grid;
}

VXGIProbeQueryParameters SSGIHybridMakeQueryParameters(SSGIHybridParameters parameters)
{
    VXGIProbeQueryParameters query = (VXGIProbeQueryParameters)0;
    query.gridOrigin = parameters.gridOrigin;
    query.cellSize = parameters.cellSize;
    query.cellDimensions = parameters.cellDimensions;
    query.probeCount = parameters.probeCount;
    query.boundsMaximum = parameters.boundsMaximum;
    query.triangleCount = parameters.triangleCount;
    query.voxelSize = parameters.voxelSize;
    query.materialCount = parameters.materialCount;
    query.materialMipLevel = parameters.materialMipLevel;
    return query;
}
#endif

bool SSGINormalizeDirection(float3 direction, out float3 unitDirection)
{
    unitDirection = 0.0;
    float maximum = VXGIProbeMaximumCoordinate(direction);
    if (!all(isfinite(direction)) || maximum == 0.0) return false;
    float3 scaled = direction / maximum;
    float lengthSquared = dot(scaled, scaled);
    if (!isfinite(lengthSquared) || lengthSquared <= 0.0) return false;
    unitDirection = scaled * rsqrt(lengthSquared);
    return all(isfinite(unitDirection));
}

#ifndef VXGI_SSGI_THRESHOLD
// Historical raster-validation helpers only. The production SSGI fallback does
// not call these functions or re-anchor the original SSGI ray to a surface.
// Strict matching for an exact geometric endpoint; raster depth reconstruction
// is not an exact endpoint, so the diagnostic below uses a projected-depth proof.
bool SSGIPrimarySurfaceMatch(float3 nearPosition, float3 targetPosition,
    SSGIHybridParameters parameters, StructuredBuffer<float> nodes,
    StructuredBuffer<uint> offsets, StructuredBuffer<uint> references,
    StructuredBuffer<VXGISDFTriangle> triangles, StructuredBuffer<VXGIMaterialData> materials,
    SamplerState materialSampler, out VXGISDFHit hit, out uint anchorStatus)
{
    hit = (VXGISDFHit)0;
    anchorStatus = SSGI_ANCHOR_INVALID;
    VXGISDFGrid grid = SSGIHybridMakeGrid(parameters);
    float3 delta = targetPosition - nearPosition;
    float distance = length(delta);
    float3 direction;
    if (!VXGIProbeQueryGridValid(grid) || parameters.triangleCount == 0
        || !all(isfinite(nearPosition)) || !all(isfinite(targetPosition))
        || !isfinite(distance) || distance <= 0.0 || !SSGINormalizeDirection(delta, direction))
        return false;
    float tolerance = VXGIProbeQueryEndpointTolerance(targetPosition, nearPosition, distance, parameters.cellSize);
    if (!isfinite(tolerance) || !isfinite(distance + tolerance)) return false;
    bool found = VXGISDFTrace(nearPosition, direction, 0.0, distance + tolerance,
        grid, nodes, offsets, references, triangles, materials, parameters.materialCount,
        materialSampler, parameters.materialMipLevel, hit);
    if (!found || hit.traceStatus != 1)
    {
        anchorStatus = hit.traceStatus == 3 ? SSGI_ANCHOR_EXHAUSTED
            : hit.traceStatus == 2 ? SSGI_ANCHOR_INVALID : SSGI_ANCHOR_MISS;
        return false;
    }
    bool matches = hit.triangleIndex < parameters.triangleCount
        && abs(hit.distance - distance) <= tolerance
        && length(hit.position - targetPosition) <= tolerance;
    anchorStatus = matches ? SSGI_ANCHOR_VERIFIED : SSGI_ANCHOR_MISMATCH;
    return matches;
}

// Prove the raster sample against the actual first surface on its camera ray.
// D32 perspective inversion loses world precision at distance, so compare the
// first hit's forward-projected depth instead of the reconstructed world point.
// No preceding hit is skipped. A depth bin (and this bounded two-ULP comparison)
// cannot recover the raster primitive identity of unresolved coincident layers;
// that requires a primitive ID in the GBuffer. This helper is retained for the
// historical raster-validation fixture, not for the production GI path.
bool SSGIPrimarySurfaceMatchDepth(float3 nearPosition, float3 targetPosition,
    float2 expectedUV, float deviceDepth, uint2 sceneSize, float4x4 viewMatrix,
    float4x4 projectionMatrix, SSGIHybridParameters parameters,
    StructuredBuffer<float> nodes, StructuredBuffer<uint> offsets,
    StructuredBuffer<uint> references, StructuredBuffer<VXGISDFTriangle> triangles,
    StructuredBuffer<VXGIMaterialData> materials, SamplerState materialSampler,
    out VXGISDFHit hit, out uint anchorStatus)
{
    hit = (VXGISDFHit)0;
    anchorStatus = SSGI_ANCHOR_INVALID;
    VXGISDFGrid grid = SSGIHybridMakeGrid(parameters);
    float3 direction;
    if (!VXGIProbeQueryGridValid(grid) || parameters.triangleCount == 0
        || any(sceneSize == 0) || !all(isfinite(expectedUV))
        || any(expectedUV < 0.0) || any(expectedUV > 1.0)
        || !isfinite(deviceDepth) || deviceDepth < 0.0 || deviceDepth > 1.0
        || !all(isfinite(nearPosition)) || !all(isfinite(targetPosition))
        || !SSGINormalizeDirection(targetPosition - nearPosition, direction))
        return false;

    // Triangle inequality bounds every point in the scene AABB, including a
    // true hit just beyond a depth sample rounded toward the camera.
    float maximumDistance = length(parameters.boundsMaximum - parameters.gridOrigin)
        + length(nearPosition - parameters.gridOrigin) + parameters.cellSize;
    if (!isfinite(maximumDistance) || maximumDistance <= 0.0) return false;
    bool found = VXGISDFTrace(nearPosition, direction, 0.0, maximumDistance,
        grid, nodes, offsets, references, triangles, materials, parameters.materialCount,
        materialSampler, parameters.materialMipLevel, hit);
    if (!found || hit.traceStatus != 1)
    {
        anchorStatus = hit.traceStatus == 3 ? SSGI_ANCHOR_EXHAUSTED
            : hit.traceStatus == 2 ? SSGI_ANCHOR_INVALID : SSGI_ANCHOR_MISS;
        return false;
    }

    float4 clip = mul(mul(float4(hit.position, 1.0), viewMatrix), projectionMatrix);
    if (!all(isfinite(clip)) || clip.w <= 0.0) return false;
    float3 ndc = clip.xyz / clip.w;
    if (!all(isfinite(ndc)) || ndc.z < 0.0 || ndc.z > 1.0) return false;
    float2 actualUV = ndc.xy * float2(0.5, -0.5) + 0.5;
    if (!all(isfinite(actualUV)) || any(actualUV < 0.0) || any(actualUV > 1.0)) return false;
    float2 uvTolerance = max(8.0 * 1.1920928955078125e-7, 0.01 / float2(sceneSize));
    // Nonnegative finite depth bit patterns are monotonically ordered. Normalize
    // signed zero before comparing; the subtraction cannot overflow in [0,1].
    uint expectedBits = deviceDepth == 0.0 ? 0u : asuint(deviceDepth);
    uint actualBits = ndc.z == 0.0 ? 0u : asuint(ndc.z);
    uint depthULPs = max(actualBits, expectedBits) - min(actualBits, expectedBits);
    bool matches = hit.triangleIndex < parameters.triangleCount
        && all(abs(actualUV - expectedUV) <= uvTolerance) && depthULPs <= 2u;
    anchorStatus = matches ? SSGI_ANCHOR_VERIFIED : SSGI_ANCHOR_MISMATCH;
    return matches;
}
#endif

// The fallback reads cached radiance after a finite world-space query. A
// surface hit directly reads the existing probe radiance. Cache lookup does
// not trace probe visibility, shade the surface, trace its sunlight or expand
// the segment to the scene bounds.
SSGIHybridRayResult SSGIWorldRaySource(float3 origin, float3 direction, float rayLength,
    SSGIHybridParameters parameters, StructuredBuffer<float> nodes,
#ifndef VXGI_SSGI_THRESHOLD
    StructuredBuffer<uint> offsets, StructuredBuffer<uint> references,
    StructuredBuffer<VXGISDFTriangle> triangles, StructuredBuffer<VXGIMaterialData> materials,
#endif
    StructuredBuffer<VXGIProbeCellEntry> lookup, StructuredBuffer<VXGIProbeRecord> rawRecords
#ifndef VXGI_SSGI_THRESHOLD
    , SamplerState materialSampler
#endif
#ifdef VXGI_SSGI_GLOBAL
    , SSGIGlobalParameters global
#endif
    )
{
    SSGIHybridRayResult result = (SSGIHybridRayResult)0;
    result.backend = SSGI_BACKEND_WORLD;
    result.triangleIndex = VXGI_PROBE_MISSING_INDEX;
    result.status = SSGI_SCREEN_INVALID;
    result.worldTraceStatus = 2;
    float3 unitDirection;
    if (!all(isfinite(origin))
        || !SSGINormalizeDirection(direction, unitDirection)
        || !isfinite(rayLength)
#ifdef VXGI_SSGI_GLOBAL
        || (rayLength <= 0.0 && (rayLength < 0.0 || global.enabled == 0))
#else
        || rayLength <= 0.0
#endif
        )
        return result;
#ifdef VXGI_SSGI_THRESHOLD
    VXGIMeshSDFThresholdCounts counts;
    counts.nodeCount = parameters.nodeCount;
    counts.assetCount = parameters.assetCount;
    counts.instanceCount = parameters.instanceCount;
    counts.bvhNodeCount = parameters.bvhNodeCount;
    VXGIThresholdParameters traceParameters;
    traceParameters.hitThresholdCells = SSGI_WORLD_HIT_THRESHOLD_CELLS;
    traceParameters.stepScale = 0.9;
    traceParameters.maxSteps = 512;
    traceParameters.padding = 0;
    VXGIMeshSDFThresholdResult hit;
#ifdef VXGI_SSGI_GLOBAL
    // A near-plane clip or receiver offset can consume the entire short
    // segment. This is an empty near interval, not a failed geometry query.
    if (rayLength == 0.0)
    {
        hit = (VXGIMeshSDFThresholdResult)0;
        hit.status = VXGI_THRESHOLD_MISS;
    }
    else
#endif
    hit = VXGIMeshSDFTraceThreshold(origin, unitDirection,
        0.0, rayLength, counts, traceParameters, nodes);
    result.worldTraceStatus = hit.status;
    bool found = hit.status == VXGI_THRESHOLD_HIT;
    if (!found)
    {
        result.status = hit.status >= VXGI_THRESHOLD_INVALID ? SSGI_SCREEN_INVALID : SSGI_SCREEN_MISS;
#ifdef VXGI_SSGI_GLOBAL
        // Only a completed clear near segment continues. Invalid/exhausted
        // mesh queries are not evidence that geometry was absent.
        if (hit.status == VXGI_THRESHOLD_MISS && global.enabled != 0)
        {
            if (!isfinite(global.maxTraceDistance) || global.maxTraceDistance < 0.0
                || global.maxSteps == 0)
            {
                result.backend = SSGI_BACKEND_GLOBAL;
                result.status = SSGI_SCREEN_INVALID;
                result.worldTraceStatus = VXGI_GLOBAL_SDF_INVALID;
                return result;
            }
            if (global.maxTraceDistance <= rayLength) return result;
            Texture3D<float> field = ResourceDescriptorHeap[global.textureIndex];
            result.backend = SSGI_BACKEND_GLOBAL;
            result.worldTraceStatus = VXGI_GLOBAL_SDF_INVALID;
            result.status = SSGI_SCREEN_INVALID;
            float3 farDirection;
            if (!VXGIGlobalSDFValidTexture(field, global.constants) ||
                !VXGIThresholdNormalize(unitDirection, farDirection)) return result;
            float farStart, farEnd;
            result.worldTraceStatus = VXGI_GLOBAL_SDF_MISS;
            result.status = SSGI_SCREEN_MISS;
            // Fine verification owns no coverage beyond the last GDF layer.
            // Use the same normalized ray as both distance-field queries.
            if (!VXGIGlobalSDFRayBounds(origin, farDirection,
                global.constants.clipmaps[global.constants.clipmapCount - 1],
                rayLength, global.maxTraceDistance, farStart, farEnd)) return result;
            uint remainingGlobalSteps = global.maxSteps;
            uint remainingFineSteps = 512;
            [loop] for (uint window = 0; window < 8; ++window)
            {
                if (remainingGlobalSteps == 0 || remainingFineSteps == 0) break;
                VXGIGlobalSDFTraceResult coarse = VXGIGlobalSDFTrace(field, global.constants,
                    origin, unitDirection, farStart, farEnd, remainingGlobalSteps);
                remainingGlobalSteps -= min(remainingGlobalSteps, coarse.sampleCount);
                result.worldTraceStatus = coarse.status;
                result.distance = coarse.distance;
                result.status = coarse.status >= VXGI_GLOBAL_SDF_INVALID ? SSGI_SCREEN_INVALID : SSGI_SCREEN_MISS;
                if (coarse.status != VXGI_GLOBAL_SDF_HIT) return result;

                // The nearest tight box proves only the clear gap before it.
                // Never treat the closest-two Mesh SDF MISS as proof that all
                // farther instances are empty.
                VXGIMeshSDFThresholdCandidates boxes = VXGIMeshSDFThresholdClosestTwo(
                    origin, farDirection, coarse.distance, farEnd, counts);
                if (boxes.valid == 0)
                {
                    result.worldTraceStatus = VXGI_THRESHOLD_INVALID;
                    result.status = SSGI_SCREEN_INVALID;
                    return result;
                }
                if (boxes.count == 0)
                {
                    result.worldTraceStatus = VXGI_GLOBAL_SDF_MISS;
                    result.status = SSGI_SCREEN_MISS;
                    result.distance = farEnd;
                    return result;
                }
                float fineStart = max(coarse.distance, boxes.candidates[0].entryDistance);
                float fineEnd = min(farEnd, fineStart + 4.0 *
                    global.constants.clipmaps[coarse.clipmapIndex].cellSize);
                VXGIThresholdParameters fineParameters = traceParameters;
                fineParameters.maxSteps = remainingFineSteps;
                VXGIMeshSDFThresholdResult refined = VXGIMeshSDFTraceThresholdAll(
                    origin, unitDirection, fineStart, fineEnd, counts, fineParameters, nodes);
                remainingFineSteps -= min(remainingFineSteps, refined.sampleCount);
                result.worldTraceStatus = refined.status;
                result.distance = refined.distance;
                result.status = refined.status >= VXGI_THRESHOLD_INVALID ? SSGI_SCREEN_INVALID : SSGI_SCREEN_MISS;
                if (refined.status == VXGI_THRESHOLD_HIT)
                {
                    result.hit = 1;
                    result.status = SSGI_SCREEN_HIT;
                    result.confidence = 1.0;
                    // A verified fine hit ends the ray regardless of cache
                    // support or RGB. Radiance never decides whether to retry.
                    VXGIProbeRadianceQueryResult farQuery = VXGIProbeQueryRadiance(refined.position,
                        unitDirection, parameters.voxelSize, parameters.probeCount, lookup, rawRecords);
                    result.probeHasSupport = farQuery.hasSupport;
                    result.probeSupportWeight = farQuery.supportWeight;
                    result.probeAcceptedCount = farQuery.acceptedCount;
                    result.probeTraceFailures = farQuery.traceFailures;
                    if (farQuery.hasSupport != 0 && all(isfinite(farQuery.radiance)))
                    {
                        result.radiance = farQuery.radiance;
                        result.sourceValid = 1;
                    }
                    return result;
                }
                if (refined.status != VXGI_THRESHOLD_MISS) return result;
                result.distance = fineEnd;
                if (fineEnd >= farEnd) return result;
                // Only a completed all-instance MISS allows this window to
                // be skipped, even when the rejected coarse point had SH.
                if (!(fineEnd > farStart)) break;
                farStart = fineEnd;
            }
            result.worldTraceStatus = VXGI_GLOBAL_SDF_EXHAUSTED;
            result.status = SSGI_SCREEN_INVALID;
        }
#endif
        return result;
    }
#else
    VXGISDFGrid grid = SSGIHybridMakeGrid(parameters);
    if (!VXGIProbeQueryGridValid(grid)) return result;
    VXGISDFHit hit;
    bool found = VXGISDFTrace(origin, unitDirection, 0.0, rayLength,
        grid, nodes, offsets, references, triangles, materials, parameters.materialCount,
        materialSampler, parameters.materialMipLevel, hit);
    result.worldTraceStatus = hit.traceStatus;
    if (!found)
    {
        result.status = hit.traceStatus >= 2 ? SSGI_SCREEN_INVALID : SSGI_SCREEN_MISS;
        return result;
    }
#endif
    result.hit = 1;
    result.status = SSGI_SCREEN_HIT;
    result.confidence = 1.0;
    result.distance = hit.distance;
    // A geometric hit terminates even when no probe supports it or its cached
    // radiance is black. Never continue behind it looking for a brighter hit.
#ifndef VXGI_SSGI_THRESHOLD
    result.triangleIndex = hit.triangleIndex;
    if (hit.triangleIndex >= parameters.triangleCount) return result;
#endif
    VXGIProbeRadianceQueryResult query = VXGIProbeQueryRadiance(hit.position, unitDirection,
        parameters.voxelSize, parameters.probeCount, lookup, rawRecords);
    result.probeHasSupport = query.hasSupport;
    result.probeSupportWeight = query.supportWeight;
    result.probeAcceptedCount = query.acceptedCount;
    result.probeTraceFailures = query.traceFailures;
    if (query.hasSupport != 0 && all(isfinite(query.radiance)))
    {
        result.radiance = query.radiance;
        result.sourceValid = 1;
    }
    return result;
}

#ifdef VXGI_SSGI_THRESHOLD
// Only models containing the original launch point can cause an immediate
// threshold hit. Use their local spacing and conservative instance scale;
// distant candidate models must not enlarge the receiver's launch offset.
// A zero-length interval makes this a point query, independent of ray direction.
float SSGIWorldReceiverNormalBias(float3 origin, SSGIHybridParameters parameters,
    float screenNormalBias)
{
    VXGIMeshSDFThresholdCounts counts;
    counts.nodeCount = parameters.nodeCount;
    counts.assetCount = parameters.assetCount;
    counts.instanceCount = parameters.instanceCount;
    counts.bvhNodeCount = parameters.bvhNodeCount;
    VXGIMeshSDFThresholdCandidates candidates = VXGIMeshSDFThresholdClosestTwo(
        origin, float3(1, 0, 0), 0.0, 0.0, counts);
    float bias = screenNormalBias;
    if (candidates.valid == 0) return bias;
    [loop] for (uint i = 0; i < candidates.count; ++i)
    {
        VXGIMeshSDFInstance instance = VXGIMeshSDFInstances[candidates.candidates[i].instanceIndex];
        if (instance.assetIndex >= parameters.assetCount) continue;
        VXGIMeshSDFAsset asset = VXGIMeshSDFAssets[instance.assetIndex];
        // Instance transforms are TRS. The largest basis length also covers
        // nonuniform scale, instead of assuming that local units equal world units.
        float maximumScale = max(length(instance.worldFromLocal[0].xyz),
            max(length(instance.worldFromLocal[1].xyz), length(instance.worldFromLocal[2].xyz)));
        float threshold = SSGI_WORLD_HIT_THRESHOLD_CELLS * asset.cellSize * maximumScale;
        if (isfinite(threshold) && threshold > 0.0)
            bias = max(bias, threshold * SSGI_WORLD_NORMAL_BIAS_SCALE);
    }
    return bias;
}
#endif

#endif
