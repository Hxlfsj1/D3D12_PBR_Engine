#include "VXGISDFCommon.hlsli"

// Headless numerical acceptance uses the production trace and surface resolver.
// These bindings belong to the standalone test, not the renderer's root signature.
cbuffer TestConstants : register(b0)
{
    float3 gridOrigin; float gridCellSize;
    uint3 gridCellDimensions; uint rayCount;
    float3 gridBoundsMaximum; uint constantsPadding;
};

struct TestRay
{
    float3 origin; float maximumDistance;
    float3 direction; float minimumDistance;
};
struct TestResult
{
    float3 position; float distance;
    float3 geometricNormal; uint valid;
    float3 barycentric; uint triangleId;
    uint triangleIndex; uint backFace; uint materialId; uint instanceId;
    float2 uv; float2 uvPadding;
    float3 shadingNormal; uint traceStatus;
    float3 tangent; float tangentPadding;
    float3 bitangent; float bitangentPadding;
};

StructuredBuffer<float> nodes : register(t0);
StructuredBuffer<uint> cellOffsets : register(t1);
StructuredBuffer<uint> triangleReferences : register(t2);
StructuredBuffer<VXGISDFTriangle> triangles : register(t3);
StructuredBuffer<TestRay> rays : register(t4);
RWStructuredBuffer<TestResult> results : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= rayCount) return;
    VXGISDFGrid grid;
    grid.origin = gridOrigin;
    grid.cellSize = gridCellSize;
    grid.cellDimensions = gridCellDimensions;
    grid.boundsMaximum = gridBoundsMaximum;
    TestRay ray = rays[dispatchId.x];
    TestResult result = (TestResult)0;
    VXGISDFHit hit;
    bool valid = VXGISDFTrace(ray.origin, ray.direction, ray.minimumDistance, ray.maximumDistance,
        grid, nodes, cellOffsets, triangleReferences, triangles, hit);
    result.traceStatus = hit.traceStatus;
    if (valid)
    {
        VXGISDFSurface surface = VXGISDFResolveSurface(hit, triangles);
        result.position = hit.position;
        result.distance = hit.distance;
        result.geometricNormal = hit.geometricNormal;
        result.valid = hit.valid;
        result.barycentric = hit.barycentric;
        result.triangleId = hit.triangleId;
        result.triangleIndex = hit.triangleIndex;
        result.backFace = hit.backFace;
        result.materialId = surface.materialId;
        result.instanceId = surface.instanceId;
        result.uv = surface.uv;
        result.shadingNormal = surface.shadingNormal;
        result.tangent = surface.tangent;
        result.bitangent = surface.bitangent;
    }
    results[dispatchId.x] = result;
}
