#include "VXGISDFCommon.hlsli"
#include "SurfaceCacheQuery.hlsli"
#include "GISourceCommon.hlsli"

cbuffer LightingConstants : register(b0)
{
    uint4 atlasIndices;
    uint outputIndex,historyIndex,padding1,padding2;
    float3 directionTowardLight;uint cardCount;
    float3 sunRadiance;float rayBias;
    float3 gridOrigin;float cellSize;
    uint3 cellDimensions;float rayMax;
    float3 boundsMaximum;uint materialCount;
};
cbuffer CardConstants : register(b1)
{
    uint captureCardIndex;
    uint updateX,updateY,updateWidth,updateHeight;
    uint oldX,oldY,oldWidth,oldHeight;
};
StructuredBuffer<float> distanceNodes : register(t0);
StructuredBuffer<uint> cellOffsets : register(t1);
StructuredBuffer<uint> triangleReferences : register(t2);
StructuredBuffer<VXGISDFTriangle> triangles : register(t3);
StructuredBuffer<VXGIMaterialData> materials : register(t4);
SamplerState materialSampler : register(s0);

[numthreads(8,8,1)]
void CSMain(uint3 id:SV_DispatchThreadID)
{
    if(any(id.xy>=uint2(updateWidth,updateHeight)))return;
    id.xy+=uint2(updateX,updateY);
    RWTexture2D<float4> output=ResourceDescriptorHeap[outputIndex];
    SurfaceCacheCard card=surfaceCacheCards[captureCardIndex];
    uint2 pixel=card.atlasOrigin+id.xy;
    uint2 materialPixel=card.materialOrigin+id.xy;
    output[pixel]=0;
    Texture2D<float> depthAtlas=ResourceDescriptorHeap[atlasIndices.w];
    float depth=depthAtlas.Load(int3(materialPixel,0));
    if(depth>=1)return;
    Texture2D<float4> emissiveAtlas=ResourceDescriptorHeap[atlasIndices.z];
    float4 emissive=emissiveAtlas.Load(int3(materialPixel,0));
    if(emissive.a<0.5)return;
    float2 uv=(float2(id.xy)+0.5)/card.resolution;
    float3 local=card.origin+card.axisX*((2*uv.x-1)*card.extentX)+
        card.axisY*((1-2*uv.y)*card.extentY)+card.axisZ*((1-2*depth)*card.extentZ);
    VXGIMeshSDFInstance instance=VXGIMeshSDFInstances[card.instanceIndex];
    float3 position=mul(float4(local,1),instance.worldFromLocal).xyz;
    Texture2D<float4> normalAtlas=ResourceDescriptorHeap[atlasIndices.y];
    float3 normal=SurfaceCacheWorldNormal(card,instance,normalAtlas.Load(int3(materialPixel,0)).xyz);
    float NdotL=dot(normal,directionTowardLight);
    float visibility=1;
    if(NdotL>0)
    {
        VXGISDFGrid grid;grid.origin=gridOrigin;grid.cellSize=cellSize;
        grid.cellDimensions=cellDimensions;grid.boundsMaximum=boundsMaximum;
        VXGISDFHit blocker;
        VXGISDFTrace(position+normal*rayBias,directionTowardLight,0,rayMax,grid,distanceNodes,cellOffsets,
            triangleReferences,triangles,materials,materialCount,materialSampler,0,blocker);
        visibility=blocker.traceStatus==0?1:0;
    }
    Texture2D<float4> albedoAtlas=ResourceDescriptorHeap[atlasIndices.x];
    float4 albedo=albedoAtlas.Load(int3(materialPixel,0));
    float3 radiance=GIComputeSourceRadiance(albedo.rgb,albedo.a,sunRadiance,NdotL,visibility,emissive.rgb,false);
    output[pixel]=float4(radiance,visibility);
}

// Layout changes keep the same Card-local UVs. Clamp to texel centers inside
// the old rectangle so the bilinear footprint never borrows another Card.
[numthreads(8,8,1)]
void ResampleCS(uint3 id:SV_DispatchThreadID)
{
    SurfaceCacheCard card=surfaceCacheCards[captureCardIndex];
    if(any(id.xy>=card.resolution))return;
    Texture2D<float4> history=ResourceDescriptorHeap[historyIndex];
    RWTexture2D<float4> output=ResourceDescriptorHeap[outputIndex];
    uint width,height;history.GetDimensions(width,height);
    uint4 oldRect=uint4(oldX,oldY,oldWidth,oldHeight);
    float2 oldPixel=float2(oldRect.xy)+(float2(id.xy)+0.5)/card.resolution*oldRect.zw;
    oldPixel=clamp(oldPixel,float2(oldRect.xy)+0.5,float2(oldRect.xy+oldRect.zw)-0.5);
    output[card.atlasOrigin+id.xy]=history.SampleLevel(materialSampler,oldPixel/float2(width,height),0);
}
