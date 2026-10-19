// Invoke the real production shader functions. No copied screen/SDF traversal.
#define SSGI_PASS_TRACE 1
#include "Shaders_For_SSGI.hlsl"

struct HybridTestRay
{
    uint2 receiver; uint padding0; uint padding1;
    float3 viewOrigin; uint fallbackEnabled;
    float3 viewDirection; uint operation;
    float3 worldOrigin; float rayLength;
    float3 worldDirection; uint padding3;
};
cbuffer HybridTestConstants : register(b1)
{
    uint testRayCount; uint testPadding0; uint testPadding1; uint testPadding2;
};
StructuredBuffer<HybridTestRay> testRays : register(t8);
RWStructuredBuffer<SSGIHybridRayResult> testResults : register(u0);

[numthreads(64, 1, 1)]
void CSMain_Rays(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= testRayCount) return;
    HybridTestRay ray = testRays[id.x];
    SSGIHybridRayResult result = (SSGIHybridRayResult)0;
    if (ray.operation == 1)
    {
        result = SSGIWorldRaySource(ray.worldOrigin, ray.worldDirection, ray.rayLength, hybrid,
            hybridNodes, hybridOffsets, hybridReferences, hybridTriangles,
            hybridMaterials, hybridLookup, hybridRawRecords,
            hybridMaterialSampler);
    }
    else if (ray.operation == 2)
    {
        Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
        Texture2D<float4> normals = ResourceDescriptorHeap[normalTextureIdx];
        float3 viewPosition = ReconstructPosition((ray.receiver + 0.5) / sceneSize,
            depth.Load(int3(ray.receiver, 0)), invProjMat);
        float3 n = SSGIDecodeViewNormal(normals.Load(int3(ray.receiver, 0)).xyz, viewMat);
        result = TraceHybridRay(ray.receiver, viewPosition + n * normalBias, ray.viewDirection);
    }
    else if (ray.operation == 3)
    {
        float3 worldOrigin, unitWorldDirection;
        result.hit = SSGIWorldRaySegment(ray.viewOrigin, ray.viewDirection,
            worldOrigin, unitWorldDirection, result.distance) ? 1u : 0u;
        result.radiance = worldOrigin;
    }
    else if (ray.operation == 4)
    {
        float3 worldOrigin, unitWorldDirection;
        result.hit = SSGIWorldRaySegment(ray.viewOrigin, ray.viewDirection,
            worldOrigin, unitWorldDirection, result.distance) ? 1u : 0u;
        result.radiance = unitWorldDirection;
    }
    else
    {
        result = TraceHybridRay(ray.receiver, ray.viewOrigin, ray.viewDirection);
    }
    testResults[id.x] = result;
}
