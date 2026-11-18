#include "VXGISDFCommon.hlsli"
#include "VXGIGlobalSDFCommon.hlsli"
#include "SurfaceCacheQuery.hlsli"
#include "FullscreenTriangle.hlsli"

cbuffer QueryDebugConstants : register(b0)
{
    float4x4 inverseViewProjection;
    float3 gridOrigin;float cellSize;
    uint3 cellDimensions;float rayMax;
    float3 boundsMaximum;uint materialCount;
    uint4 atlasIndices;
    uint queryMode,channel,bvhNodeCount,globalIndex;
    float2 inverseOutputSize;float positionTolerance;uint globalMaxSteps;
    uint lightingIndex;uint3 queryPadding;
    VXGIGlobalSDFConstants global;
};
StructuredBuffer<float> distanceNodes : register(t0);
StructuredBuffer<uint> cellOffsets : register(t1);
StructuredBuffer<uint> triangleReferences : register(t2);
StructuredBuffer<VXGISDFTriangle> triangles : register(t3);
StructuredBuffer<VXGIMaterialData> materials : register(t4);
SamplerState materialSampler : register(s0);

float4 VSMain(uint id:SV_VertexID):SV_POSITION
{return GetFullscreenTrianglePosition(GetFullscreenTriangleTexCoord(id));}

struct QueryOutput
{
    float4 preview:SV_Target0;
    float4 albedoMetallic:SV_Target1;
    float4 normalRoughness:SV_Target2;
    float4 emissiveSource:SV_Target3;
    float4 queryPosition:SV_Target4;
    float4 address:SV_Target5;
    float4 surfacePosition:SV_Target6;
    float4 radianceVisibility:SV_Target7;
};
QueryOutput PSMain(float4 pixel:SV_POSITION)
{
    QueryOutput output=(QueryOutput)0;
    const float3 background=float3(0.018,0.022,0.03);
    output.preview=float4(background,1);output.address=float4(-1,-1,0,0);
    float2 ndc=pixel.xy*inverseOutputSize*float2(2,-2)+float2(-1,1);
    float4 nearH=mul(float4(ndc,0,1),inverseViewProjection),farH=mul(float4(ndc,1,1),inverseViewProjection);
    float3 rayOrigin=nearH.xyz/nearH.w,rayDirection=normalize(farH.xyz/farH.w-rayOrigin);
    float3 position,normal;uint instanceIndex=0xffffffffu;float tolerance=positionTolerance;
    if(queryMode==28)
    {
        Texture3D<float> field=ResourceDescriptorHeap[globalIndex];
        VXGIGlobalSDFTraceResult hit=VXGIGlobalSDFTrace(field,global,rayOrigin,rayDirection,0,rayMax,globalMaxSteps);
        if(hit.status!=VXGI_GLOBAL_SDF_HIT)return output;
        position=hit.position;
        float h=global.clipmaps[hit.clipmapIndex].cellSize*0.5;
        float3 gradient=0;
        [unroll] for(uint axis=0;axis<3;++axis)
        {
            float3 delta=0;delta[axis]=h;
            gradient[axis]=VXGIGlobalSDFSampleClipmapUnchecked(field,global,hit.clipmapIndex,position+delta).distance-
                VXGIGlobalSDFSampleClipmapUnchecked(field,global,hit.clipmapIndex,position-delta).distance;
        }
        normal=dot(gradient,gradient)>1e-12?normalize(gradient):-rayDirection;
        // Explicit coarse diagnostic neighborhood, not an exact surface hit.
        tolerance=2*(global.clipmaps[hit.clipmapIndex].cellSize+cellSize);
    }
    else
    {
        VXGISDFGrid grid;grid.origin=gridOrigin;grid.cellSize=cellSize;
        grid.cellDimensions=cellDimensions;grid.boundsMaximum=boundsMaximum;
        VXGISDFHit hit;
        if(!VXGISDFTrace(rayOrigin,rayDirection,0,rayMax,grid,distanceNodes,cellOffsets,
            triangleReferences,triangles,materials,materialCount,materialSampler,0,hit))return output;
        position=hit.position;normal=hit.geometricNormal;
        if(queryMode==26)instanceIndex=hit.instanceIndex;
    }
    output.queryPosition=float4(position,1);
    SurfaceCacheSample sample;
    if(!SurfaceCacheQuery(position,normal,instanceIndex,tolerance,atlasIndices,lightingIndex,bvhNodeCount,sample))
    {output.preview=float4(1,0,1,1);return output;}
    output.albedoMetallic=float4(sample.albedo,sample.metallic);
    output.normalRoughness=float4(sample.normal,sample.roughness);
    output.emissiveSource=float4(sample.emissive,sample.sourceEnabled);
    output.address=float4(sample.instanceIndex,sample.cardIndex,sample.uv);
    output.surfacePosition=float4(sample.position,1);
    output.radianceVisibility=float4(sample.radiance,sample.visibility);
    float3 color=pow(max(sample.albedo,0),1.0/2.2);
    if(channel==1)color=sample.normal*0.5+0.5;
    if(channel==2)color=pow(max(sample.emissive,0)/(1+max(sample.emissive,0)),1.0/2.2);
    if(channel==3)color=0.2+0.75*frac(float(sample.instanceIndex*32+sample.cardIndex+1)*float3(.61803399,.38196601,.75487767));
    if(channel==4)color=pow(max(sample.radiance,0)/(1+max(sample.radiance,0)),1.0/2.2);
    if(channel==5)color=sample.visibility.xxx;
    output.preview=float4(color,1);return output;
}
