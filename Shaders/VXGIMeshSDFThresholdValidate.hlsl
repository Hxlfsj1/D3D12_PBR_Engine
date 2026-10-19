#include "SSGIHybridCommon.hlsli"

cbuffer TestConstants : register(b0)
{
    SSGIHybridParameters hybrid;
    uint rayCount; uint3 padding;
};
struct TestRay
{
    float3 origin; float minimum;
    float3 direction; float maximum;
    VXGIThresholdParameters parameters;
    uint operation; uint3 padding;
};
struct TestResult
{
    float3 position; float distance;
    float fieldDistance; uint status; uint instanceIndex; uint instanceId;
    uint sampleCount; uint candidateCount; uint2 padding;
    SSGIHybridRayResult hybrid;
};
StructuredBuffer<float> nodes : register(t0);
StructuredBuffer<TestRay> rays : register(t1);
StructuredBuffer<VXGIProbeCellEntry> lookup : register(t2);
StructuredBuffer<VXGIProbeRecord> raw : register(t3);
RWStructuredBuffer<TestResult> results : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= rayCount) return;
    TestRay ray = rays[id.x];
    TestResult value = (TestResult)0;
    if (ray.operation == 0)
    {
        VXGIMeshSDFThresholdCounts counts;
        counts.nodeCount = hybrid.nodeCount;
        counts.assetCount = hybrid.assetCount;
        counts.instanceCount = hybrid.instanceCount;
        counts.bvhNodeCount = hybrid.bvhNodeCount;
        VXGIMeshSDFThresholdResult hit = VXGIMeshSDFTraceThreshold(ray.origin,
            ray.direction, ray.minimum, ray.maximum, counts, ray.parameters, nodes);
        value.position = hit.position;
        value.distance = hit.distance;
        value.fieldDistance = hit.fieldDistance;
        value.status = hit.status;
        value.instanceIndex = hit.instanceIndex;
        value.instanceId = hit.instanceId;
        value.sampleCount = hit.sampleCount;
        value.candidateCount = hit.candidateCount;
    }
    else
    {
        value.hybrid = SSGIWorldRaySource(ray.origin, ray.direction, ray.maximum,
            hybrid, nodes, lookup, raw);
    }
    results[id.x] = value;
}
