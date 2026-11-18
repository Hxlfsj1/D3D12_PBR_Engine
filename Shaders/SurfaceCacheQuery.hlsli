#ifndef SURFACE_CACHE_QUERY_HLSLI
#define SURFACE_CACHE_QUERY_HLSLI

#include "VXGIMeshSDFData.hlsli"
#include "GISourceCommon.hlsli"

struct SurfaceCacheCard
{
    float3 origin; uint instanceIndex;
    float3 axisX; float extentX;
    float3 axisY; float extentY;
    float3 axisZ; float extentZ;
    uint2 atlasOrigin; uint2 resolution;
    uint cardIndex,feedbackIndex;
    uint2 materialOrigin; uint materialInstanceIndex;
};
StructuredBuffer<SurfaceCacheCard> surfaceCacheCards : register(t0, space2);
StructuredBuffer<uint2> surfaceCacheRanges : register(t1, space2);

struct SurfaceCacheSample
{
    float3 position,normal,albedo,emissive;
    float3 radiance,sourceEmission;
    float visibility;
    float metallic,roughness,sourceEnabled;
    float2 uv;
    uint instanceIndex,cardIndex,feedbackIndex;
    float quality;
};

float3 SurfaceCacheWorldNormal(SurfaceCacheCard card,VXGIMeshSDFInstance instance,float3 capturedNormal)
{
    if(card.materialInstanceIndex==card.instanceIndex)return normalize(capturedNormal);
    VXGIMeshSDFInstance owner=VXGIMeshSDFInstances[card.materialInstanceIndex];
    float3 local=mul(capturedNormal,transpose((float3x3)owner.worldFromLocal));
    return normalize(mul(local,transpose((float3x3)instance.localFromWorld)));
}

#if SURFACE_CACHE_FEEDBACK
void SurfaceCacheRequest(uint index);
#endif

// The caller supplies a world position, a unit surface normal and its world
// position uncertainty. Exact Mesh/screen points use a small epsilon; coarse
// distance-field hits explicitly allow a larger neighborhood.
void SurfaceCacheQueryInstance(float3 position,float3 normal,uint instanceIndex,float positionTolerance,
    uint4 atlasIndices,uint lightingIndex,inout SurfaceCacheSample best)
{
    VXGIMeshSDFInstance instance=VXGIMeshSDFInstances[instanceIndex];
    float3 localPosition=mul(float4(position,1),instance.localFromWorld).xyz;
    float3 localNormal=normalize(mul(normal,transpose((float3x3)instance.worldFromLocal)));
    uint2 range=surfaceCacheRanges[instanceIndex];
    Texture2D<float4> albedoAtlas=ResourceDescriptorHeap[atlasIndices.x];
    Texture2D<float4> normalAtlas=ResourceDescriptorHeap[atlasIndices.y];
    Texture2D<float4> emissiveAtlas=ResourceDescriptorHeap[atlasIndices.z];
    Texture2D<float> depthAtlas=ResourceDescriptorHeap[atlasIndices.w];
    Texture2D<float4> lightingAtlas=ResourceDescriptorHeap[lightingIndex];
    [loop] for(uint index=range.x;index<range.x+range.y;++index)
    {
        SurfaceCacheCard card=surfaceCacheCards[index];
        float facing=dot(localNormal,card.axisZ);
        if(facing<0.25)continue;
        float3 offset=localPosition-card.origin;
        float3 p=float3(dot(offset,card.axisX),dot(offset,card.axisY),dot(offset,card.axisZ));
        float2 uv=float2(p.x/card.extentX,-p.y/card.extentY)*0.5+0.5;
        if(any(uv<0)||any(uv>1))continue;
        if(card.resolution.x==0)
        {
#if SURFACE_CACHE_FEEDBACK
            // Keep only the description after eviction. A geometry hit inside
            // this Card's slab can request its rectangle without reading Atlas.
            if(abs(p.z)<=card.extentZ+positionTolerance)SurfaceCacheRequest(card.feedbackIndex);
#endif
            continue;
        }

        // A tilted plane changes depth within a texel. Account for that footprint
        // before rejecting the four taps; this keeps planar interpolation exact.
        float2 texelSize=2*float2(card.extentX,card.extentY)/card.resolution;
        float slope=dot(abs(float2(dot(localNormal,card.axisX),dot(localNormal,card.axisY))),texelSize)/facing;
        float localError=positionTolerance*length(mul((float3x3)instance.localFromWorld,card.axisZ));
        float tolerance=localError+slope+max(1e-5,card.extentZ*1e-5);
        float2 pixel=uv*card.resolution-0.5;
        int2 base=int2(floor(pixel));float2 f=frac(pixel);
        float weightSum=0,depthSum=0;float4 albedoSum=0,normalSum=0,emissiveSum=0;
        float4 lightingSum=0;float3 sourceEmissionSum=0;
        [unroll] for(uint y=0;y<2;++y) [unroll] for(uint x=0;x<2;++x)
        {
            // Clamp each tap to its own tile. Other Cards never enter a filter.
            uint2 offsetInCard=uint2(clamp(base+int2(x,y),int2(0,0),int2(card.resolution)-1));
            uint2 texel=card.materialOrigin+offsetInCard;
            float depth=depthAtlas.Load(int3(texel,0));
            if(depth>=1||abs((1-2*depth)*card.extentZ-p.z)>tolerance)continue;
            float4 n=normalAtlas.Load(int3(texel,0));
            n.xyz=SurfaceCacheWorldNormal(card,instance,n.xyz);
            if(dot(n.xyz,normal)<0.25)continue;
            float2 weights=lerp(1-f,f,float2(x,y));float weight=weights.x*weights.y;
            weightSum+=weight;depthSum+=depth*weight;normalSum+=n*weight;
            albedoSum+=albedoAtlas.Load(int3(texel,0))*weight;
            float4 emission=emissiveAtlas.Load(int3(texel,0));
            emissiveSum+=emission*weight;lightingSum+=lightingAtlas.Load(int3(card.atlasOrigin+offsetInCard,0))*weight;
            sourceEmissionSum+=GICapSourceEmissive(emission.rgb)*emission.a*weight;
        }
        if(weightSum<=1e-6)continue;
        float capturedZ=(1-2*depthSum/weightSum)*card.extentZ;
        float3 sampledNormal=normalize(normalSum.xyz);
        float quality=facing*facing*dot(sampledNormal,normal)/
            (1+abs(capturedZ-p.z)/tolerance);
        if(quality<=best.quality)continue;
        float3 capturedLocal=card.origin+card.axisX*p.x+card.axisY*p.y+card.axisZ*capturedZ;
        best.position=mul(float4(capturedLocal,1),instance.worldFromLocal).xyz;
        best.normal=sampledNormal;best.albedo=albedoSum.rgb/weightSum;best.metallic=albedoSum.a/weightSum;
        best.roughness=normalSum.a/weightSum;best.emissive=emissiveSum.rgb/weightSum;
        best.sourceEnabled=emissiveSum.a/weightSum;best.uv=uv;
        best.radiance=lightingSum.rgb/weightSum;best.visibility=lightingSum.a/weightSum;
        best.sourceEmission=sourceEmissionSum/weightSum;
        best.instanceIndex=instanceIndex;best.cardIndex=card.cardIndex;best.quality=quality;
        best.feedbackIndex=card.feedbackIndex;
    }
}

// UINT_MAX means ownership is unknown (screen points or Global SDF hits).
// Traverse all overlapping leaves of the existing instance BVH, without a fixed
// candidate cap. A known owner never borrows another instance's material.
bool SurfaceCacheQuery(float3 position,float3 normal,uint instanceIndex,float positionTolerance,
    uint4 atlasIndices,uint lightingIndex,uint bvhNodeCount,out SurfaceCacheSample sample)
{
    sample=(SurfaceCacheSample)0;
    sample.instanceIndex=sample.cardIndex=0xffffffffu;
    if(instanceIndex!=0xffffffffu)
        SurfaceCacheQueryInstance(position,normal,instanceIndex,positionTolerance,atlasIndices,lightingIndex,sample);
    else
    {
        uint index=0;
        [loop] while(index<bvhNodeCount)
        {
            VXGIMeshSDFBVHNode node=VXGIMeshSDFBVH[index];
            float3 delta=position-clamp(position,node.boundsMinimum,node.boundsMaximum);
            if(dot(delta,delta)>positionTolerance*positionTolerance){index=node.escapeIndex;continue;}
            if(node.instanceIndex!=0xffffffffu)
                SurfaceCacheQueryInstance(position,normal,node.instanceIndex,positionTolerance,atlasIndices,lightingIndex,sample);
            ++index;
        }
    }
    return sample.quality>0;
}

#endif
