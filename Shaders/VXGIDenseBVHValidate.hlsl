#include "VXGISDFCommon.hlsli"

cbuffer TestConstants : register(b0)
{
    float3 gridOrigin; float cellSize;
    uint3 cellDimensions; uint triangleCount;
    float3 boundsMaximum; uint materialCount;
    uint rayCount; uint3 padding;
};
struct TestRay
{
    float3 origin; float minimum;
    float3 direction; float maximum;
    float mip; uint3 padding;
};
struct TestHit
{
    float3 position; float distance;
    float3 barycentric; uint triangleIndex;
    float3 normal; uint triangleId;
    uint backFace; uint valid; uint status; uint reserved;
};
struct TestPair { TestHit baseline; TestHit accelerated; };
StructuredBuffer<float> nodes : register(t0);
StructuredBuffer<uint> linearOffsets : register(t1);
StructuredBuffer<uint> linearReferences : register(t2);
StructuredBuffer<uint> packedOffsets : register(t3);
StructuredBuffer<uint> packedReferences : register(t4);
StructuredBuffer<VXGISDFTriangle> triangles : register(t5);
StructuredBuffer<VXGIMaterialData> materials : register(t6);
StructuredBuffer<TestRay> rays : register(t7);
RWStructuredBuffer<TestPair> results : register(u0);
SamplerState materialSampler : register(s0);

TestHit ConvertHit(VXGISDFHit hit)
{
    TestHit value = (TestHit)0;
    value.position = hit.position; value.distance = hit.distance;
    value.barycentric = hit.barycentric; value.triangleIndex = hit.triangleIndex;
    value.normal = hit.geometricNormal; value.triangleId = hit.triangleId;
    value.backFace = hit.backFace; value.valid = hit.valid; value.status = hit.traceStatus;
    return value;
}
[numthreads(64,1,1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if(id.x >= rayCount) return;
    VXGISDFGrid grid;
    grid.origin = gridOrigin; grid.cellSize = cellSize;
    grid.cellDimensions = cellDimensions; grid.boundsMaximum = boundsMaximum;
    TestRay ray = rays[id.x];
    VXGISDFHit baseline, accelerated;
    VXGISDFTrace(ray.origin, ray.direction, ray.minimum, ray.maximum,
        grid, nodes, linearOffsets, linearReferences, triangles, materials,
        materialCount, materialSampler, ray.mip, baseline);
    VXGISDFTrace(ray.origin, ray.direction, ray.minimum, ray.maximum,
        grid, nodes, packedOffsets, packedReferences, triangles, materials,
        materialCount, materialSampler, ray.mip, accelerated);
    TestPair value;
    value.baseline = ConvertHit(baseline); value.accelerated = ConvertHit(accelerated);
    results[id.x] = value;
}
