#ifndef SCREEN_PROBE_WORLD_EMISSIVE_HLSLI
#define SCREEN_PROBE_WORLD_EMISSIVE_HLSLI

#define SP_EMISSIVE_SAMPLES emissiveSamples

uint ProbeRandomHash(uint x)
{x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16);}

struct ProbeEmissiveSample{float3 radiance;uint cell;float distance;};

// Uniform physical-triangle selection and uniform barycentric area sampling.
// PDF(point)=1/(triangleCount * triangleArea). The directional bins store mean
// Li; SH integration supplies receiver cosine and the 2*pi/64 solid angle later.
ProbeEmissiveSample SampleWorldEmissive(uint2 seed,uint lane,float3 p,float3 launchNormal,float3 normal)
{
    ProbeEmissiveSample result=(ProbeEmissiveSample)0;result.cell=0xffffffffu;result.distance=worldTraceDistance;
    if(lane>=SP_EMISSIVE_SAMPLES)return result;
    uint random=ProbeRandomHash(seed.x+seed.y*outputSize.x+frameIndex*0x9e3779b9u+lane*0x85ebca6bu);
    // Upper 32 bits of random*count, without an optional int64 shader feature.
    uint lo=random&0xffffu,hi=random>>16;
    uint t=hi*(worldEmitterCount&0xffffu)+((lo*(worldEmitterCount&0xffffu))>>16);
    uint selected=hi*(worldEmitterCount>>16)+(t>>16)+(((t&0xffffu)+lo*(worldEmitterCount>>16))>>16);
    uint2 emitter=worldEmitters[selected];VXGISDFTriangle surfaceTriangle=worldTriangles[emitter.y];
    VXGIMeshSDFInstance instance=VXGIMeshSDFInstances[emitter.x];
    float3 a=mul(float4(surfaceTriangle.p0,1),instance.worldFromLocal).xyz;
    float3 b=mul(float4(surfaceTriangle.p1,1),instance.worldFromLocal).xyz;
    float3 c=mul(float4(surfaceTriangle.p2,1),instance.worldFromLocal).xyz;
    float3 crossEdges=cross(b-a,c-a);float twiceArea=length(crossEdges);
    float3 lightNormal=crossEdges/twiceArea;
    float2 xi=float2(ProbeRandomHash(random^0x68bc21ebu)>>8,ProbeRandomHash(random^0x02e5be93u)>>8)/16777216.0;
    float root=sqrt(xi.x);float3 bary=float3(1-root,root*(1-xi.y),root*xi.y);
    float3 target=a*bary.x+b*bary.y+c*bary.z;
    float3 origin=p+launchNormal*normalBias;float3 toLight=target-origin;float distance=length(toLight);
    if(distance<.001||distance>worldTraceDistance)return result;
    float3 direction=toLight/distance;float mu=dot(normal,direction),cosine=dot(lightNormal,-direction);
    if(mu<=0||cosine<=0)return result;
    SurfaceCacheSample sample;
    if(!SSGIReadCache(target,lightNormal,emitter.x,.001,sample)||!any(sample.sourceEmission>0))return result;
    if(!SSGIWorldVisible(origin,direction,distance-worldRayBias*4))return result;
    float solidAngleWeight=float(worldEmitterCount)*(twiceArea*.5)*cosine/(distance*distance);
    result.radiance=sample.sourceEmission*(solidAngleWeight*(64.0/6.28318530718)/float(SP_EMISSIVE_SAMPLES));
    float3 tangent=normalize(cross(abs(normal.z)<.999?float3(0,0,1):float3(0,1,0),normal));
    float phi=atan2(dot(direction,cross(normal,tangent)),dot(direction,tangent));if(phi<0)phi+=6.28318530718;
    uint2 cell=min(uint2(mu*8,phi*(8.0/6.28318530718)),7u);
    result.cell=cell.y*8u+cell.x;result.distance=distance;
    SPCounter(probeDiagnosticsIndexPlusOne,21u,1u);return result;
}
groupshared float3 sharedEmissiveRadiance[64];
groupshared uint sharedEmissiveCell[64];
groupshared float sharedEmissiveDistance[64];
#endif
