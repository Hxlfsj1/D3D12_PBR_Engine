#include "VXGISDFThresholdCommon.hlsli"

cbuffer TestConstants : register(b0)
{
    uint rayCount; uint nodeCount; uint2 padding;
};
struct TestRay
{
    VXGIThresholdGrid grid;
    VXGIThresholdParameters parameters;
    float3 origin; float minimum;
    float3 direction; float maximum;
};
struct TestResult
{
    float3 position; float distance;
    float fieldDistance; uint status; uint sampleCount; uint padding;
};
StructuredBuffer<float> nodes : register(t0);
StructuredBuffer<TestRay> rays : register(t1);
RWStructuredBuffer<TestResult> results : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= rayCount) return;
    TestRay ray = rays[id.x];
    VXGIThresholdResult hit = VXGIThresholdTraceLocal(nodes, nodeCount,
        ray.grid, ray.origin, ray.direction, ray.minimum, ray.maximum, ray.parameters);
    TestResult value = (TestResult)0;
    value.position = hit.position;
    value.distance = hit.distance;
    value.fieldDistance = hit.fieldDistance;
    value.status = hit.status;
    value.sampleCount = hit.sampleCount;
    results[id.x] = value;
}
