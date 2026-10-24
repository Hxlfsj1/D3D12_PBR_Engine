#include "../Shaders/VXGIGlobalSDFCommon.hlsli"

cbuffer TestLayout : register(b0) { VXGIGlobalSDFConstants Layout; }
cbuffer TestCount : register(b1) { uint QueryCount; }
Texture3D<float> Field : register(t0);
struct Query
{
    float3 origin; float rayMinimum;
    float3 direction; float rayMaximum;
    uint maximumSteps; uint mode; uint2 padding;
};
struct Result
{
    float3 position; uint status;
    float distance; float fieldDistance; float cellSize; uint clipmapIndex;
    uint sampleCount; uint valid; float safeDistance; uint padding;
};
StructuredBuffer<Query> Queries : register(t1);
RWStructuredBuffer<Result> Results : register(u0);
[numthreads(64,1,1)]
void CSQuery(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= QueryCount) return;
    Query q = Queries[id.x];
    Result r = (Result)0;
    if (q.mode == 0)
    {
        VXGIGlobalSDFSample s = VXGIGlobalSDFSampleDistance(Field, Layout, q.origin);
        r.position = q.origin;
        r.distance = s.distance;
        r.safeDistance = s.safeDistance;
        r.cellSize = s.cellSize;
        r.clipmapIndex = s.clipmapIndex;
        r.valid = s.valid;
    }
    else
    {
        VXGIGlobalSDFTraceResult t = VXGIGlobalSDFTrace(Field, Layout, q.origin,
            q.direction, q.rayMinimum, q.rayMaximum, q.maximumSteps);
        r.position = t.position;
        r.status = t.status;
        r.distance = t.distance;
        r.fieldDistance = t.fieldDistance;
        r.clipmapIndex = t.clipmapIndex;
        r.sampleCount = t.sampleCount;
    }
    Results[id.x] = r;
}
