#include "VXGIProbeQueryCommon.hlsli"
cbuffer FilterConstants : register(b0) { VXGIProbeQueryParameters parameters; };
cbuffer ValidationConstants : register(b1) { uint queryCount; uint rawResetMode; uint padding0; uint padding1; };
StructuredBuffer<float> nodes : register(t0);
StructuredBuffer<uint> offsets : register(t1);
StructuredBuffer<uint> references : register(t2);
StructuredBuffer<VXGISDFTriangle> triangles : register(t3);
StructuredBuffer<float3> centers : register(t4);
StructuredBuffer<VXGIProbeCellEntry> lookup : register(t5);
StructuredBuffer<VXGIProbeNeighborRecord> neighbors : register(t6);
StructuredBuffer<VXGIProbeRecord> raw : register(t7);
StructuredBuffer<uint> visibility : register(t8);
StructuredBuffer<VXGIMaterialData> materials : register(t9);
SamplerState materialSampler : register(s0);
struct QueryInput
{
    float3 position; uint triangleIndex;
    float3 geometricNormal; uint backFace;
    float3 mappedNormal; uint valid;
    float3 barycentric; uint triangleId;
    float distance; uint traceStatus; uint padding0; uint padding1;
};
StructuredBuffer<QueryInput> queries : register(t10);
RWStructuredBuffer<VXGIProbeRecord> resetRecords : register(u3);
RWStructuredBuffer<VXGIProbeQueryResult> results : register(u2);
[numthreads(64,1,1)]
void CSQuery(uint3 id : SV_DispatchThreadID)
{
    if(id.x>=queryCount)return;
    QueryInput input=queries[id.x];VXGISDFHit hit=(VXGISDFHit)0;
    hit.position=input.position;hit.distance=input.distance;hit.geometricNormal=input.geometricNormal;
    hit.triangleId=input.triangleId;hit.triangleIndex=input.triangleIndex;hit.barycentric=input.barycentric;
    hit.backFace=input.backFace;hit.valid=input.valid;hit.traceStatus=input.traceStatus;
    results[id.x]=VXGIProbeQuerySurface(hit,input.mappedNormal,parameters,lookup,centers,raw,nodes,offsets,references,triangles,
        materials,materialSampler);
}
// Reuse the 48-byte diagnostic output layout for directional radiance tests.
// mappedNormal is deliberately the requested direction in this entry point.
// No hit identity, geometry, normal, material or center is provided to the query.
[numthreads(64,1,1)]
void CSQueryRadiance(uint3 id : SV_DispatchThreadID)
{
    if(id.x>=queryCount)return;
    QueryInput input=queries[id.x];
    VXGIProbeRadianceQueryResult source=VXGIProbeQueryRadiance(input.position,input.mappedNormal,
        parameters.voxelSize,parameters.probeCount,lookup,raw);
    VXGIProbeQueryResult result;
    result.irradianceOverPi=source.radiance;result.supportWeight=source.supportWeight;
    result.unclampedIrradianceOverPi=source.unclampedRadiance;result.negativeMask=source.negativeMask;
    result.candidateCount=source.candidateCount;result.acceptedCount=source.acceptedCount;
    result.hasSupport=source.hasSupport;result.traceFailures=source.traceFailures;
    results[id.x]=result;
}
// The reset implementation is the same public function called by CSReset.
// Rebinding this GPU result as t7 checks that queries reject cleared history.
[numthreads(64,1,1)]
void CSResetRaw(uint3 id : SV_DispatchThreadID)
{
    if(id.x>=parameters.probeCount)return;
    resetRecords[id.x]=VXGIProbeResetRecord(raw[id.x],rawResetMode);
}
