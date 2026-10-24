#define VXGI_SSGI_THRESHOLD 1
#ifndef HYBRID_LEGACY
#define VXGI_SSGI_GLOBAL 1
#endif
#include "../Shaders/SSGIHybridCommon.hlsli"

// Both variants use the same CPU buffer. The legacy entry independently
// compiles the unchanged near-only function signature and implementation.
cbuffer TestParameters : register(b0)
{
    SSGIHybridParameters Parameters;
#ifdef VXGI_SSGI_GLOBAL
    SSGIGlobalParameters Global;
#endif
}
cbuffer TestCount : register(b1) { uint QueryCount; }
struct Query
{
    float3 origin; float nearLength;
    float3 direction; float farLength;
    uint enabled; uint maximumSteps; uint invalidNear; uint padding;
};
StructuredBuffer<float> Nodes : register(t0);
StructuredBuffer<VXGIProbeCellEntry> Lookup : register(t6);
StructuredBuffer<VXGIProbeRecord> RawSH : register(t7);
StructuredBuffer<Query> Queries : register(t8);
RWStructuredBuffer<SSGIHybridRayResult> Results : register(u0);
[numthreads(64,1,1)]
void CSHybrid(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= QueryCount) return;
    Query q = Queries[id.x];
    SSGIHybridParameters p = Parameters;
    if (q.invalidNear != 0) p.nodeCount = 0;
#ifdef VXGI_SSGI_GLOBAL
    SSGIGlobalParameters g = Global;
    g.enabled = q.enabled;
    g.maxTraceDistance = q.farLength;
    g.maxSteps = q.maximumSteps;
    if (q.padding != 0)
    {
        // Diagnostic oracle for coarse-shell regressions: show that the
        // original GDF candidate alone would have supplied cached radiance.
        // The production hybrid below must still verify real Mesh SDF geometry.
        Texture3D<float> field = ResourceDescriptorHeap[g.textureIndex];
        VXGIGlobalSDFTraceResult coarse = VXGIGlobalSDFTrace(field, g.constants,
            q.origin, q.direction, q.nearLength, q.farLength, q.maximumSteps);
        SSGIHybridRayResult r = (SSGIHybridRayResult)0;
        r.backend = SSGI_BACKEND_GLOBAL;
        r.distance = coarse.distance;
        r.worldTraceStatus = coarse.status;
        r.anchorStatus = coarse.sampleCount;
        if (coarse.status == VXGI_GLOBAL_SDF_HIT)
        {
            r.hit = 1;
            r.status = SSGI_SCREEN_HIT;
            VXGIProbeRadianceQueryResult source = VXGIProbeQueryRadiance(coarse.position,
                q.direction, p.voxelSize, p.probeCount, Lookup, RawSH);
            r.probeHasSupport = source.hasSupport;
            r.probeSupportWeight = source.supportWeight;
            r.probeAcceptedCount = source.acceptedCount;
            if (source.hasSupport != 0 && all(isfinite(source.radiance)))
            {
                r.radiance = source.radiance;
                r.sourceValid = 1;
            }
        }
        Results[id.x] = r;
        return;
    }
    Results[id.x] = SSGIWorldRaySource(q.origin, q.direction, q.nearLength,
        p, Nodes, Lookup, RawSH, g);
#else
    Results[id.x] = SSGIWorldRaySource(q.origin, q.direction, q.nearLength,
        p, Nodes, Lookup, RawSH);
#endif
}
