#include "VXGISurfaceCommon.hlsli"

cbuffer TestConstants : register(b0)
{
    float3 gridOrigin; float gridCellSize;
    uint3 gridCellDimensions; uint rayCount;
    float3 gridBoundsMaximum; uint materialCount;
};
struct TestRay
{
    float3 origin; float maximumDistance;
    float3 direction; float minimumDistance;
    float3 sunDirection; float materialMipLevel;
    float3 sunRadiance; float shadowMinimumDistance;
    float shadowMaximumDistance; uint3 padding;
};
struct TestResult
{
    float3 albedo; float alpha;
    float3 orm; float perceptualRoughness;
    float3 shadingNormal; float metallic;
    float3 uncappedEmissive; uint materialId;
    float3 directDiffuse; float sunVisibility;
    float3 limitedEmissive; uint sourceValid;
    float3 lo0; uint backFace;
    float3 screenLo0; uint instanceId;
    float2 uv; uint hitValid; uint traceStatus;
    float3 position; float distance;
    float3 geometricNormal; uint shadowTraceStatus;
    float occlusion; uint isUnlit; uint2 padding;
};
StructuredBuffer<float> nodes : register(t0);
StructuredBuffer<uint> cellOffsets : register(t1);
StructuredBuffer<uint> triangleReferences : register(t2);
StructuredBuffer<VXGISDFTriangle> triangles : register(t3);
StructuredBuffer<VXGIMaterialData> materials : register(t4);
StructuredBuffer<TestRay> rays : register(t5);
RWStructuredBuffer<TestResult> results : register(u0);
SamplerState materialSampler : register(s0);

[numthreads(64,1,1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    if(dispatchId.x>=rayCount) return;
    VXGISDFGrid grid;
    grid.origin=gridOrigin;grid.cellSize=gridCellSize;
    grid.cellDimensions=gridCellDimensions;grid.boundsMaximum=gridBoundsMaximum;
    TestRay ray=rays[dispatchId.x];TestResult result=(TestResult)0;VXGISDFHit hit;
    if(VXGISDFTrace(ray.origin,ray.direction,ray.minimumDistance,ray.maximumDistance,
        grid,nodes,cellOffsets,triangleReferences,triangles,materials,materialCount,materialSampler,ray.materialMipLevel,hit))
    {
        VXGISurfaceSource source=VXGIEvaluateSurfaceSource(hit,grid,nodes,cellOffsets,
            triangleReferences,triangles,materials,materialCount,materialSampler,ray.materialMipLevel,
            ray.sunDirection,ray.sunRadiance,ray.shadowMinimumDistance,ray.shadowMaximumDistance);
        result.albedo=source.surface.albedo;result.alpha=source.surface.alpha;
        result.orm=source.surface.orm;result.perceptualRoughness=source.surface.perceptualRoughness;
        result.shadingNormal=source.surface.shadingNormal;result.metallic=source.surface.metallic;
        result.uncappedEmissive=source.surface.uncappedEmissive;result.materialId=source.surface.materialId;
        result.directDiffuse=source.directDiffuse;result.sunVisibility=source.sunVisibility;
        result.limitedEmissive=source.limitedEmissive;result.sourceValid=source.valid;
        result.lo0=source.lo0;result.backFace=source.surface.backFace;result.instanceId=source.surface.instanceId;
        // The screen source uses this same production formula with its GBuffer inputs.
        float NdotL=max(dot(source.surface.shadingNormal,normalize(ray.sunDirection)),0);
        result.screenLo0=GIComputeSourceRadiance(source.surface.albedo,source.surface.metallic,
            ray.sunRadiance,NdotL,source.sunVisibility,source.surface.uncappedEmissive,source.surface.isUnlit!=0);
        result.uv=source.surface.uv;result.hitValid=hit.valid;
        result.position=hit.position;result.distance=hit.distance;result.geometricNormal=hit.geometricNormal;
        result.shadowTraceStatus=source.shadowTraceStatus;result.occlusion=source.surface.occlusion;
        result.isUnlit=source.surface.isUnlit;
    }
    result.traceStatus=hit.traceStatus;
    results[dispatchId.x]=result;
}
