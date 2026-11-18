#ifndef SSGI_SURFACE_CACHE_HLSLI
#define SSGI_SURFACE_CACHE_HLSLI

#define VXGI_MESH_SDF 1
#include "VXGISDFCommon.hlsli"
#include "VXGIGlobalSDFCommon.hlsli"
#define SURFACE_CACHE_FEEDBACK 1
#include "SurfaceCacheQuery.hlsli"

cbuffer SSGIWorldConstants:register(b1)
{
    float3 worldGridOrigin;float worldCellSize;
    uint3 worldCellDimensions;float worldMeshDistance;
    float3 worldBoundsMaximum;float worldTraceDistance;
    uint4 worldAtlasIndices;
    uint worldLightingIndex,worldGlobalIndex,worldBVHNodeCount,worldGlobalEnabled;
    float worldRayBias;uint worldMaterialCount,worldEmitterCount,worldLumen;
    uint worldFeedbackIndex;uint3 worldFeedbackPadding;
    VXGIGlobalSDFConstants worldGlobal;
};
StructuredBuffer<float> worldDistanceNodes:register(t0);
StructuredBuffer<uint> worldCellOffsets:register(t1);
StructuredBuffer<uint> worldTriangleReferences:register(t2);
StructuredBuffer<VXGISDFTriangle> worldTriangles:register(t3);
StructuredBuffer<VXGIMaterialData> worldMaterials:register(t4);
StructuredBuffer<uint2> worldEmitters:register(t2,space2);
SamplerState worldMaterialSampler:register(s0);

void SurfaceCacheRequest(uint index)
{
    RWBuffer<uint> feedback=ResourceDescriptorHeap[worldFeedbackIndex];
    InterlockedOr(feedback[index],2u);
}

VXGISDFGrid SSGIWorldGrid()
{
    VXGISDFGrid grid;grid.origin=worldGridOrigin;grid.cellSize=worldCellSize;
    grid.cellDimensions=worldCellDimensions;grid.boundsMaximum=worldBoundsMaximum;return grid;
}
bool SSGIWorldVisible(float3 origin,float3 direction,float distance)
{
    SPCounter(probeDiagnosticsIndexPlusOne,20u,1u);
    if(worldBVHNodeCount==0)return true;
    VXGISDFHit blocker;
    VXGISDFTrace(origin,direction,0,distance,SSGIWorldGrid(),worldDistanceNodes,worldCellOffsets,
        worldTriangleReferences,worldTriangles,worldMaterials,worldMaterialCount,worldMaterialSampler,0,blocker);
    return blocker.traceStatus==0;
}
bool SSGIReadCache(float3 p,float3 n,uint instance,float tolerance,out SurfaceCacheSample sample)
{
    bool found=SurfaceCacheQuery(p,n,instance,tolerance,worldAtlasIndices,worldLightingIndex,worldBVHNodeCount,sample);
    if(found)
    {
        RWBuffer<uint> feedback=ResourceDescriptorHeap[worldFeedbackIndex];
        InterlockedOr(feedback[sample.feedbackIndex],1u);
    }
    SPCounter(probeDiagnosticsIndexPlusOne,19u,found?0u:1u);return found;
}
float3 SSGICacheRadiance(SurfaceCacheSample sample,bool replaceEmission)
{
    if(replaceEmission){SPCounter(probeDiagnosticsIndexPlusOne,22u,any(sample.sourceEmission>0)?1u:0u);
        return max(sample.radiance-sample.sourceEmission,0);}
    return sample.radiance;
}

// Screen and world routes use the same cached Lo0. A geometry hit without Card
// coverage terminates the ray with zero source radiance.
float4 SSGITraceCached(uint2 receiver,float3 origin,float3 direction,bool replaceEmission)
{
    float3 viewOrigin=mul(float4(origin,1),viewMat).xyz;
    float3 viewDirection=normalize(mul(float4(direction,0),viewMat).xyz);
    SSGIScreenRayResult screen=TraceScreenRay(receiver,viewOrigin,viewDirection);
    SurfaceCacheSample sample;
    if(screen.status==SSGI_SCREEN_HIT)
    {
        SPCounter(probeDiagnosticsIndexPlusOne,16u,1u);
        Texture2D<float4> normals=ResourceDescriptorHeap[normalTextureIdx];
        float3 n=SSGIReceiverGeometricNormal(screen.hitPixel,normalize(DecodeGBufferNormal(normals.Load(int3(screen.hitPixel,0)).xyz)));
        float3 p=mul(float4(screen.viewPoint,1),invViewMat).xyz;
        if(SSGIReadCache(p,n,0xffffffffu,.001,sample))
        {SPCounter(probeDiagnosticsIndexPlusOne,23u,1u);return float4(SSGICacheRadiance(sample,replaceEmission),max(screen.distance,.001));}
        return float4(0,0,0,max(screen.distance,.001));
    }
    if(worldLumen==0 || worldBVHNodeCount==0)return float4(0,0,0,worldTraceDistance);
    VXGISDFHit hit;
    if(VXGISDFTrace(origin,direction,0,worldMeshDistance,SSGIWorldGrid(),worldDistanceNodes,worldCellOffsets,
        worldTriangleReferences,worldTriangles,worldMaterials,worldMaterialCount,worldMaterialSampler,0,hit))
    {
        SPCounter(probeDiagnosticsIndexPlusOne,17u,1u);
        if(hit.backFace==0 && SSGIReadCache(hit.position,hit.geometricNormal,hit.instanceIndex,.001,sample))
        {SPCounter(probeDiagnosticsIndexPlusOne,24u,1u);return float4(SSGICacheRadiance(sample,replaceEmission),max(hit.distance,.001));}
        return float4(0,0,0,max(hit.distance,.001));
    }
    if(hit.traceStatus!=0 || worldGlobalEnabled==0)return float4(0,0,0,worldTraceDistance);
    Texture3D<float> field=ResourceDescriptorHeap[worldGlobalIndex];
    VXGIGlobalSDFTraceResult far=VXGIGlobalSDFTrace(field,worldGlobal,origin,direction,worldMeshDistance,worldTraceDistance,256);
    if(far.status!=VXGI_GLOBAL_SDF_HIT)return float4(0,0,0,worldTraceDistance);
    SPCounter(probeDiagnosticsIndexPlusOne,18u,1u);
    // The conservative unsigned field supplies a candidate, not a surface point
    // or its owner. Confirm the remaining segment with the existing Mesh query.
    if(VXGISDFTrace(origin,direction,worldMeshDistance,worldTraceDistance,SSGIWorldGrid(),worldDistanceNodes,worldCellOffsets,
        worldTriangleReferences,worldTriangles,worldMaterials,worldMaterialCount,worldMaterialSampler,0,hit))
    {
        if(hit.backFace==0 && SSGIReadCache(hit.position,hit.geometricNormal,hit.instanceIndex,.001,sample))
        {SPCounter(probeDiagnosticsIndexPlusOne,25u,1u);return float4(SSGICacheRadiance(sample,replaceEmission),max(hit.distance,.001));}
        return float4(0,0,0,max(hit.distance,.001));
    }
    return float4(0,0,0,worldTraceDistance);
}
#endif
