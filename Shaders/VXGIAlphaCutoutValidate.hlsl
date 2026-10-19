#include "VXGIProbeQueryCommon.hlsli"

cbuffer TestConstants : register(b0)
{
    VXGIProbeQueryParameters parameters;
    uint testCount; uint3 padding;
};
struct TestRay
{
    float3 origin; float maximumDistance;
    float3 direction; float minimumDistance;
    float3 target; uint operation;
    float3 sunDirection; float mipLevel;
    float3 sunRadiance; float shadowMinimum;
    float shadowMaximum; uint targetTriangle; uint targetBackFace; uint probeIndex;
    float3 barycentric; uint reserved;
};
struct TestResult
{
    float3 position; float distance;
    uint triangleIndex; uint traceStatus; uint hitValid; uint materialId;
    float2 uv; float alpha; uint visible;
    float3 lo0; float sunVisibility;
    uint sourceValid; uint shadowTraceStatus; uint probeState; uint probeReason;
    float backfaceRatio; float minimumHitDistance; uint classificationRays; uint traceFailures;
    float3 irradiance; float supportWeight;
    uint acceptedCount; uint hasSupport; uint queryFailures; uint reserved;
};
StructuredBuffer<float> nodes : register(t0);
StructuredBuffer<uint> offsets : register(t1);
StructuredBuffer<uint> references : register(t2);
StructuredBuffer<VXGISDFTriangle> triangles : register(t3);
StructuredBuffer<VXGIMaterialData> materials : register(t4);
StructuredBuffer<TestRay> rays : register(t5);
StructuredBuffer<float3> centers : register(t6);
StructuredBuffer<VXGIProbeCellEntry> lookup : register(t7);
StructuredBuffer<VXGIProbeRecord> records : register(t8);
RWStructuredBuffer<TestResult> results : register(u0);
SamplerState materialSampler : register(s0);

[numthreads(64,1,1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if(id.x >= testCount) return;
    TestRay ray = rays[id.x];
    TestResult result = (TestResult)0;
    VXGISDFGrid grid = VXGIProbeQueryMakeGrid(parameters);
    if(ray.operation == 0 || ray.operation == 1)
    {
        VXGISDFHit hit;
        bool found = VXGISDFTrace(ray.origin, ray.direction, ray.minimumDistance, ray.maximumDistance,
            grid, nodes, offsets, references, triangles, materials, parameters.materialCount,
            materialSampler, ray.mipLevel, hit);
        result.traceStatus = hit.traceStatus;
        result.hitValid = hit.valid;
        result.triangleIndex = hit.triangleIndex;
        result.position = hit.position;
        result.distance = hit.distance;
        if(found)
        {
            VXGISDFSurface surface = VXGISDFResolveSurface(hit, triangles);
            result.materialId = surface.materialId;
            result.uv = surface.uv;
            VXGISurfaceMaterial material = VXGISampleSurfaceMaterial(hit, triangles, materials, materialSampler, ray.mipLevel);
            result.alpha = material.alpha;
            if(ray.operation == 1)
            {
                VXGISurfaceSource source = VXGIEvaluateSurfaceSource(hit, grid, nodes, offsets, references, triangles,
                    materials, parameters.materialCount, materialSampler, ray.mipLevel, ray.sunDirection,
                    ray.sunRadiance, ray.shadowMinimum, ray.shadowMaximum);
                result.lo0 = source.lo0;
                result.sunVisibility = source.sunVisibility;
                result.sourceValid = source.valid;
                result.shadowTraceStatus = source.shadowTraceStatus;
            }
        }
    }
    else if(ray.operation == 2)
    {
        bool failed;
        result.visible = VXGIProbeCenterVisibility(ray.origin, ray.target, grid, nodes, offsets,
            references, triangles, materials, parameters.materialCount, materialSampler, ray.mipLevel, failed);
        result.traceFailures = failed ? 1 : 0;
    }
    else if(ray.operation == 3 || ray.operation == 5)
    {
        VXGISDFHit hit = (VXGISDFHit)0;
        hit.position = ray.target; hit.distance = length(ray.target - ray.origin);
        hit.triangleIndex = ray.targetTriangle; hit.triangleId = triangles[ray.targetTriangle].triangleId;
        hit.geometricNormal = triangles[ray.targetTriangle].geometricNormal;
        hit.barycentric = ray.barycentric; hit.backFace = ray.targetBackFace; hit.valid = 1; hit.traceStatus = 1;
        if(ray.operation == 3)
        {
            bool failed;
            result.visible = VXGIProbeSurfaceVisibility(hit, ray.origin, grid, parameters.triangleCount,
                nodes, offsets, references, triangles, materials, parameters.materialCount,
                materialSampler, ray.mipLevel, failed);
            result.traceFailures = failed ? 1 : 0;
        }
        else
        {
            VXGIProbeQueryParameters query = parameters; query.materialMipLevel = ray.mipLevel;
            float3 mappedNormal = ray.targetBackFace == 0 ? hit.geometricNormal : -hit.geometricNormal;
            VXGIProbeQueryResult value = VXGIProbeQuerySurface(hit, mappedNormal, query,
                lookup, centers, records, nodes, offsets, references, triangles, materials, materialSampler);
            result.irradiance = value.irradianceOverPi; result.supportWeight = value.supportWeight;
            result.acceptedCount = value.acceptedCount; result.hasSupport = value.hasSupport;
            result.queryFailures = value.traceFailures;
        }
    }
    else if(ray.operation == 4)
    {
        VXGIProbeParameters classify = (VXGIProbeParameters)0;
        classify.gridOrigin = parameters.gridOrigin; classify.cellSize = parameters.cellSize;
        classify.cellDimensions = parameters.cellDimensions; classify.probeCount = parameters.probeCount;
        classify.boundsMaximum = parameters.boundsMaximum; classify.triangleCount = parameters.triangleCount;
        classify.materialCount = parameters.materialCount; classify.materialMipLevel = ray.mipLevel;
        classify.rayMinDistance = ray.minimumDistance; classify.maxRayDistance = ray.maximumDistance;
        classify.classifyRays = 64; classify.backfaceThreshold = 0.25;
        VXGIProbeRecord value = VXGIProbeClassify(ray.origin, ray.probeIndex, classify,
            nodes, offsets, references, triangles, materials, materialSampler);
        result.probeState = value.state; result.probeReason = value.reason;
        result.backfaceRatio = value.backfaceRatio; result.minimumHitDistance = value.minHitDistance;
        result.classificationRays = value.classificationRays;
    }
    results[id.x] = result;
}
