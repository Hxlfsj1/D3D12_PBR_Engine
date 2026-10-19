#include "VXGIProbeCommon.hlsli"
cbuffer DebugConstants : register(b0)
{
    float4x4 viewProjection;
    float voxelSize; uint debugMode; uint frameIndex; uint probeCount;
};
StructuredBuffer<float3> centers : register(t0);
StructuredBuffer<VXGIProbeRecord> records : register(t1);
struct VXGIProbeDebugNeighbors { uint indices[6]; }; // 24-byte sparse topology record.
StructuredBuffer<VXGIProbeDebugNeighbors> neighbors : register(t2);
StructuredBuffer<uint> visibility : register(t3);
struct Output { float4 position:SV_Position; float3 color:COLOR0; };
static const float3 axes[6]={float3(1,0,0),float3(-1,0,0),float3(0,1,0),float3(0,-1,0),float3(0,0,1),float3(0,0,-1)};
static const uint octahedron[24]={2,0,4,2,4,1,2,1,5,2,5,0,3,4,0,3,1,4,3,5,1,3,0,5};
Output VSMain(uint vertex:SV_VertexID,uint instance:SV_InstanceID)
{
    Output result;
    if(debugMode==17)
    {
        const uint edge=vertex/2;
        const uint neighbor=neighbors[instance].indices[edge];
        result.position=float4(2,2,2,1);result.color=float3(.38,.38,.38);
        // Canonical one-line representation of each undirected six-neighbor edge.
        if(neighbor==0xffffffff || neighbor<instance || neighbor>=probeCount) return result;
        result.position=mul(float4(centers[(vertex&1)==0?instance:neighbor],1),viewProjection);
        const uint mask=visibility[instance];
        if((mask&0x80000000)!=0)
            result.color=(mask&(1u<<(8+edge)))!=0?float3(1,.05,1):
                (mask&(1u<<edge))!=0?float3(.1,.9,.22):float3(1,.1,.05);
        return result;
    }
    VXGIProbeRecord record=records[instance];
    const bool shView=debugMode==14 || debugMode==16;
    float3 direction=axes[shView?octahedron[vertex]:vertex];
    result.position=mul(float4(centers[instance]+direction*voxelSize*0.18,1),viewProjection);
    result.color=record.state==VXGI_PROBE_VALID?float3(.1,.9,.22):record.state==VXGI_PROBE_DISABLED?float3(1,.1,.05):float3(.38,.38,.38);
    if(shView)
    {
        // Display only: reconstruct diffuse E/pi at the octahedron vertices,
        // then compress HDR and encode gamma. Stored radiance SH remains linear.
        float3 source=VXGIProbeReconstructIrradianceOverPi(record,direction);
        result.color=pow(source/(1+source),1.0/2.2);
        if(record.state!=VXGI_PROBE_VALID || record.updateCount==0) result.position=float4(2,2,2,1);
    }
    else if(debugMode==15 && record.state==VXGI_PROBE_VALID)
    {
        float age=saturate(float(frameIndex-record.lastUpdateFrame)/256.0);
        result.color=record.updateCount==0?float3(.38,.38,.38):lerp(float3(.05,.6,1),float3(1,.2,.02),age);
    }
    return result;
}
float4 PSMain(Output input):SV_Target { return float4(input.color,1); }
