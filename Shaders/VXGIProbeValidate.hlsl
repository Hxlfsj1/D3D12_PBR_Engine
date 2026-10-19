#include "VXGIProbeCommon.hlsli"

// Numerical helper acceptance only. Actual classification and lighting use the
// production CSReset/CSUpdate entry points in the same headless harness.
cbuffer ProbeValidationConstants : register(b0)
{
    float3 gridOrigin; float gridCellSize;
    uint3 gridCellDimensions; uint probeCount;
    float3 gridBoundsMaximum; uint triangleCount;
    float3 sunDirection; float materialMipLevel;
    float3 sunRadiance; float rayMinDistance;
    float maxRayDistance; uint raysPerProbe; uint maxHistoryUpdates; uint frameIndex;
    uint updateStart; uint updateCount; uint resetMode; uint materialCount;
    uint classifyRays; float backfaceThreshold; uint padding0; uint padding1;
};
struct NumericalResult
{
    float4 sh[4];
    float4 irradiance[6];
    float4 firstDirection;
};
RWStructuredBuffer<NumericalResult> results : register(u0);

[numthreads(64,1,1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if(id.x>=probeCount) return;
    VXGIProbeRecord record=(VXGIProbeRecord)0;
    const uint pairCount=raysPerProbe/2;
    // Case 0 is a constant RGB field. Cases 1..6 add +/- directional x/y/z.
    const uint axis=id.x==0?0:(id.x-1)/2;
    const float sign=id.x==0?0:((id.x&1)?1:-1);
    for(uint pair=0;pair<pairCount;++pair)
    {
        const float3 positive=VXGIProbeSampleDirection(pair,pairCount,0,0);
        const float3 directional=float3(0.4,0.2,0.1)*sign*positive[axis];
        VXGIProbeAccumulatePair(record,positive,float3(1.0,0.6,0.25)+directional,
            float3(1.0,0.6,0.25)-directional,raysPerProbe);
    }
    record.state=1;
    NumericalResult result=(NumericalResult)0;
    for(uint coefficient=0;coefficient<4;++coefficient)result.sh[coefficient]=record.sh[coefficient];
    for(uint normalIndex=0;normalIndex<6;++normalIndex)
    {
        float3 normal=0;
        normal[normalIndex/2]=(normalIndex&1)?-1:1;
        result.irradiance[normalIndex]=float4(VXGIProbeReconstructIrradianceOverPi(record,normal),1);
    }
    result.firstDirection=float4(VXGIProbeSampleDirection(0,pairCount,0,0),1);
    results[id.x]=result;
}
