#ifndef VXGI_PROBE_COMMON_HLSLI
#define VXGI_PROBE_COMMON_HLSLI

#include "VXGISurfaceCommon.hlsli"
#include "VXGIProbeData.hlsli"

// Eight 16-byte rows, matching the 32-DWORD production root constants.
struct VXGIProbeParameters
{
    float3 gridOrigin; float cellSize;
    uint3 cellDimensions; uint probeCount;
    float3 boundsMaximum; uint triangleCount;
    float3 sunDirectionTowardLight; float materialMipLevel;
    float3 sunRadiance; float rayMinDistance;
    float maxRayDistance; uint raysPerProbe; uint maxHistoryUpdates; uint frameIndex;
    uint updateStart; uint updateCount; uint resetMode; uint materialCount;
    uint classifyRays; float backfaceThreshold; uint padding0; uint padding1;
};

VXGISDFGrid VXGIProbeMakeGrid(VXGIProbeParameters parameters)
{
    VXGISDFGrid grid;
    grid.origin = parameters.gridOrigin;
    grid.cellSize = parameters.cellSize;
    grid.cellDimensions = parameters.cellDimensions;
    grid.boundsMaximum = parameters.boundsMaximum;
    return grid;
}

// Cosine convolution divided by PI has multipliers [1, 2/3, 2/3, 2/3].
// This is the consumption boundary; stored SH coefficients remain unclamped.
float3 VXGIProbeReconstructIrradianceOverPi(VXGIProbeRecord record, float3 normal)
{
    float maximumNormal = max(abs(normal.x), max(abs(normal.y), abs(normal.z)));
    if (!all(isfinite(normal)) || maximumNormal == 0.0) return 0.0;
    normal = normalize(normal / maximumNormal);
    float4 basis = VXGIProbeSHBasis(normal);
    float3 result = record.sh[0].rgb * basis.x
        + (2.0 / 3.0) * (record.sh[1].rgb * basis.y
            + record.sh[2].rgb * basis.z + record.sh[3].rgb * basis.w);
    return max(result, 0.0);
}

uint VXGIProbeHash32(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352d;
    value ^= value >> 15;
    value *= 0x846ca68b;
    value ^= value >> 16;
    return value;
}

float VXGIProbeHashUnit(uint value)
{
    return float(VXGIProbeHash32(value) & 0x00ffffff) * (1.0 / 16777216.0);
}

// Uniform hemisphere Fibonacci directions, paired with their exact negatives,
// cover the sphere. A hash-selected uniform quaternion rotates the whole set.
// Classification uses updateIndex=0; capture uses successful updateCount+1.
// Neither the camera nor frame scheduling participates in this rotation.
float3 VXGIProbeSampleDirection(uint pairIndex, uint pairCount, uint probeIndex, uint updateIndex)
{
    float z = (float(pairIndex) + 0.5) / float(max(pairCount, 1));
    float phi = 2.39996322972865332 * float(pairIndex);
    float sinePhi, cosinePhi;
    sincos(phi, sinePhi, cosinePhi);
    float radius = sqrt(max(0.0, 1.0 - z * z));
    float3 direction = float3(radius * cosinePhi, radius * sinePhi, z);
    uint seed = probeIndex * 0x9e3779b9 ^ updateIndex * 0x85ebca6b ^ 0x68bc21eb;
    float u1 = VXGIProbeHashUnit(seed + 0x9e3779b9);
    float u2 = VXGIProbeHashUnit(seed + 0x3c6ef372);
    float u3 = VXGIProbeHashUnit(seed + 0xdaa66d2b);
    float sine2, cosine2, sine3, cosine3;
    sincos(2.0 * PI * u2, sine2, cosine2);
    sincos(2.0 * PI * u3, sine3, cosine3);
    float4 rotation = float4(sqrt(1.0 - u1) * sine2, sqrt(1.0 - u1) * cosine2,
        sqrt(u1) * sine3, sqrt(u1) * cosine3);
    return normalize(direction + 2.0 * cross(rotation.xyz,
        cross(rotation.xyz, direction) + rotation.w * direction));
}

uint VXGIProbeRayCount(uint requestedCount)
{
    return clamp(requestedCount, 8, 256) & ~1u;
}

void VXGIProbeClearSH(inout VXGIProbeRecord record)
{
    [unroll] for (uint coefficient = 0; coefficient < 4; ++coefficient)
        record.sh[coefficient] = 0.0;
}

// Sum/difference form preserves exact zero L1 for constant paired radiance.
void VXGIProbeAccumulatePair(inout VXGIProbeRecord record, float3 direction,
    float3 positiveRadiance, float3 negativeRadiance, uint rayCount)
{
    float4 basis = VXGIProbeSHBasis(direction);
    float weight = (4.0 * PI) / float(max(rayCount, 2));
    float3 sum = positiveRadiance + negativeRadiance;
    float3 difference = positiveRadiance - negativeRadiance;
    record.sh[0].rgb += sum * (basis.x * weight);
    record.sh[1].rgb += difference * (basis.y * weight);
    record.sh[2].rgb += difference * (basis.z * weight);
    record.sh[3].rgb += difference * (basis.w * weight);
}

VXGIProbeRecord VXGIProbeResetRecord(VXGIProbeRecord previous, uint resetMode)
{
    if (resetMode == 1) return (VXGIProbeRecord)0;
    if (resetMode != 2) return previous;
    // An invalid classification or source trace is retryable after a reset.
    // A reliable backface/unsafe-origin decision remains attached to the center.
    if (previous.reason == VXGI_PROBE_REASON_CLASSIFY_TRACE_FAILURE
        || previous.reason == VXGI_PROBE_REASON_SAMPLE_TRACE_FAILURE)
        return (VXGIProbeRecord)0;
    VXGIProbeClearSH(previous);
    previous.updateCount = 0;
    previous.sampleCount = 0;
    previous.lastUpdateFrame = 0;
    return previous;
}

bool VXGIProbeTraceParametersValid(float3 center, VXGIProbeParameters parameters)
{
    return all(isfinite(center)) && all(isfinite(parameters.gridOrigin))
        && all(isfinite(parameters.boundsMaximum)) && isfinite(parameters.cellSize)
        && parameters.cellSize > 0.0 && all(parameters.cellDimensions > 0)
        && all(parameters.boundsMaximum > parameters.gridOrigin)
        && isfinite(parameters.rayMinDistance) && parameters.rayMinDistance >= 0.0
        && isfinite(parameters.maxRayDistance) && parameters.maxRayDistance >= parameters.rayMinDistance;
}

VXGIProbeRecord VXGIProbeClassify(float3 center, uint probeIndex, VXGIProbeParameters parameters,
    StructuredBuffer<float> nodes, StructuredBuffer<uint> cellOffsets,
    StructuredBuffer<uint> triangleReferences, StructuredBuffer<VXGISDFTriangle> triangles,
    StructuredBuffer<VXGIMaterialData> materials, SamplerState materialSampler)
{
    VXGIProbeRecord result = (VXGIProbeRecord)0;
    result.minHitDistance = isfinite(parameters.maxRayDistance) ? max(parameters.maxRayDistance, 0.0) : 0.0;
    if (!VXGIProbeTraceParametersValid(center, parameters) || !isfinite(parameters.backfaceThreshold))
    {
        result.state = VXGI_PROBE_DISABLED;
        result.reason = VXGI_PROBE_REASON_CLASSIFY_TRACE_FAILURE;
        return result;
    }
    uint rayCount = VXGIProbeRayCount(parameters.classifyRays);
    uint backfaceHits = 0;
    bool anyHit = false;
    bool traceFailure = false;
    VXGISDFGrid grid = VXGIProbeMakeGrid(parameters);
    [loop] for (uint pairIndex = 0; pairIndex < rayCount / 2; ++pairIndex)
    {
        float3 direction = VXGIProbeSampleDirection(pairIndex, rayCount / 2, probeIndex, 0);
        [unroll] for (uint side = 0; side < 2; ++side)
        {
            VXGISDFHit hit;
            bool found = VXGISDFTrace(center, side == 0 ? direction : -direction, 0.0,
                parameters.maxRayDistance, grid, nodes, cellOffsets, triangleReferences, triangles,
                materials, parameters.materialCount, materialSampler, parameters.materialMipLevel, hit);
            traceFailure = traceFailure || hit.traceStatus >= 2;
            if (found)
            {
                anyHit = true;
                backfaceHits += hit.backFace != 0 ? 1 : 0;
                result.minHitDistance = min(result.minHitDistance, hit.distance);
            }
        }
    }
    result.classificationRays = rayCount;
    result.backfaceRatio = float(backfaceHits) / float(rayCount);
    result.state = VXGI_PROBE_VALID;
    // Failure and an unsafe exact origin take precedence over winding diagnosis.
    if (traceFailure) result.reason = VXGI_PROBE_REASON_CLASSIFY_TRACE_FAILURE;
    else if (anyHit && result.minHitDistance <= parameters.rayMinDistance) result.reason = VXGI_PROBE_REASON_UNSAFE_ORIGIN;
    else if (result.backfaceRatio > saturate(parameters.backfaceThreshold)) result.reason = VXGI_PROBE_REASON_BACKFACE;
    if (result.reason != VXGI_PROBE_REASON_NONE) result.state = VXGI_PROBE_DISABLED;
    return result;
}

// A verified geometric miss contributes zero incident radiance. Black hits still
// evaluate a real surface and must never be interpreted as geometric misses.
bool VXGIProbeTraceSource(float3 center, float3 direction, VXGIProbeParameters parameters,
    StructuredBuffer<float> nodes, StructuredBuffer<uint> cellOffsets,
    StructuredBuffer<uint> triangleReferences, StructuredBuffer<VXGISDFTriangle> triangles,
    StructuredBuffer<VXGIMaterialData> materials, SamplerState materialSampler,
    out float3 radiance)
{
    radiance = 0.0;
    VXGISDFGrid grid = VXGIProbeMakeGrid(parameters);
    VXGISDFHit hit;
    bool found = VXGISDFTrace(center, direction, parameters.rayMinDistance, parameters.maxRayDistance,
        grid, nodes, cellOffsets, triangleReferences, triangles,
        materials, parameters.materialCount, materialSampler, parameters.materialMipLevel, hit);
    if (hit.traceStatus >= 2) return false;
    if (!found) return hit.traceStatus == 0;
    if (hit.triangleIndex >= parameters.triangleCount
        || VXGISDFHitMaterialId(hit,triangles) >= parameters.materialCount) return false;
    VXGISurfaceSource source = VXGIEvaluateSurfaceSource(hit, grid, nodes, cellOffsets,
        triangleReferences, triangles, materials, parameters.materialCount, materialSampler, parameters.materialMipLevel,
        parameters.sunDirectionTowardLight, parameters.sunRadiance,
        parameters.rayMinDistance, parameters.maxRayDistance);
    if (source.valid == 0 || !all(isfinite(source.lo0))) return false;
    radiance = source.lo0;
    return true;
}

uint VXGIProbeSaturatingAdd(uint value, uint increment)
{
    return value > 0xffffffffu - increment ? 0xffffffffu : value + increment;
}

VXGIProbeRecord VXGIProbeUpdate(VXGIProbeRecord previous, float3 center, uint probeIndex,
    VXGIProbeParameters parameters, StructuredBuffer<float> nodes, StructuredBuffer<uint> cellOffsets,
    StructuredBuffer<uint> triangleReferences, StructuredBuffer<VXGISDFTriangle> triangles,
    StructuredBuffer<VXGIMaterialData> materials, SamplerState materialSampler)
{
    if (previous.state == VXGI_PROBE_UNCLASSIFIED)
        previous = VXGIProbeClassify(center, probeIndex, parameters, nodes, cellOffsets, triangleReferences, triangles,
            materials, materialSampler);
    if (previous.state != VXGI_PROBE_VALID) return previous;
    VXGIProbeRecord estimate = (VXGIProbeRecord)0;
    uint rayCount = VXGIProbeRayCount(parameters.raysPerProbe);
    bool traceFailure = !VXGIProbeTraceParametersValid(center, parameters);
    [loop] for (uint pairIndex = 0; pairIndex < rayCount / 2 && !traceFailure; ++pairIndex)
    {
        float3 direction = VXGIProbeSampleDirection(pairIndex, rayCount / 2, probeIndex,
            VXGIProbeSaturatingAdd(previous.updateCount, 1));
        float3 positiveRadiance, negativeRadiance;
        bool positiveValid = VXGIProbeTraceSource(center, direction, parameters, nodes, cellOffsets,
            triangleReferences, triangles, materials, materialSampler, positiveRadiance);
        bool negativeValid = VXGIProbeTraceSource(center, -direction, parameters, nodes, cellOffsets,
            triangleReferences, triangles, materials, materialSampler, negativeRadiance);
        traceFailure = !positiveValid || !negativeValid;
        if (!traceFailure)
            VXGIProbeAccumulatePair(estimate, direction, positiveRadiance, negativeRadiance, rayCount);
    }
    if (traceFailure)
    {
        VXGIProbeClearSH(previous);
        previous.state = VXGI_PROBE_DISABLED;
        previous.reason = VXGI_PROBE_REASON_SAMPLE_TRACE_FAILURE;
        return previous;
    }
    uint historyUpdates = min(VXGIProbeSaturatingAdd(previous.updateCount, 1), max(parameters.maxHistoryUpdates, 1));
    float newWeight = 1.0 / float(historyUpdates);
    [unroll] for (uint coefficient = 0; coefficient < 4; ++coefficient)
    {
        previous.sh[coefficient].rgb = lerp(previous.sh[coefficient].rgb, estimate.sh[coefficient].rgb, newWeight);
        previous.sh[coefficient].w = 0.0;
    }
    previous.updateCount = VXGIProbeSaturatingAdd(previous.updateCount, 1);
    previous.sampleCount = VXGIProbeSaturatingAdd(previous.sampleCount, rayCount);
    previous.lastUpdateFrame = parameters.frameIndex;
    previous.reason = VXGI_PROBE_REASON_NONE;
    return previous;
}

#endif
