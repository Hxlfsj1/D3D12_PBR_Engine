#define VXGI_MESH_SDF 1
#include "VXGISDFCommon.hlsli"

cbuffer TestConstants : register(b0)
{
    uint rayCount; uint materialCount; uint geometryOnly; uint padding;
};
struct TestRay
{
    float3 origin; float minimum;
    float3 direction; float maximum;
};
struct TestHit
{
    float3 position; float distance;
    float3 barycentric; uint triangleIndex;
    float3 geometricNormal; uint triangleId;
    float3 shadingNormal; uint materialId;
    uint instanceId; uint instanceIndex; uint backFace; uint valid;
    uint status; uint isCutout; uint2 reserved;
};
StructuredBuffer<float> nodes : register(t0);
StructuredBuffer<uint> offsets : register(t1);
StructuredBuffer<uint> references : register(t2);
StructuredBuffer<VXGISDFTriangle> triangles : register(t3);
StructuredBuffer<VXGIMaterialData> materials : register(t4);
StructuredBuffer<TestRay> rays : register(t5);
RWStructuredBuffer<TestHit> results : register(u0);
SamplerState materialSampler : register(s0);

[numthreads(64,1,1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if(id.x >= rayCount) return;
    TestRay ray = rays[id.x];
    VXGISDFGrid unusedGrid = (VXGISDFGrid)0;
    VXGISDFHit hit;
    if(geometryOnly != 0)
        VXGISDFTrace(ray.origin, ray.direction, ray.minimum, ray.maximum,
            unusedGrid, nodes, offsets, references, triangles, hit);
    else
        VXGISDFTrace(ray.origin, ray.direction, ray.minimum, ray.maximum,
            unusedGrid, nodes, offsets, references, triangles, materials,
            materialCount, materialSampler, 0.0, hit);
    TestHit value = (TestHit)0;
    value.position = hit.position; value.distance = hit.distance;
    value.barycentric = hit.barycentric; value.triangleIndex = hit.triangleIndex;
    value.geometricNormal = hit.geometricNormal; value.triangleId = hit.triangleId;
    value.instanceIndex = hit.instanceIndex; value.backFace = hit.backFace;
    value.valid = hit.valid; value.status = hit.traceStatus;
    if(hit.valid != 0)
    {
        VXGISDFSurface surface = VXGISDFResolveSurface(hit, triangles);
        value.shadingNormal = surface.shadingNormal;
        value.materialId = surface.materialId; value.instanceId = surface.instanceId;
        value.isCutout = VXGISDFHitIsCutout(hit, triangles) ? 1 : 0;
    }
    results[id.x] = value;
}
