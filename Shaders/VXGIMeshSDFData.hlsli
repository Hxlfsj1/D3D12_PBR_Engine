#ifndef VXGI_MESH_SDF_DATA_HLSLI
#define VXGI_MESH_SDF_DATA_HLSLI

// Immutable model-space assets and transform-only scene instances. This shared
// layout is independent of triangle tracing and can be read by the GI consumer.
struct VXGIMeshSDFAsset
{
    float3 origin; float cellSize;
    uint3 cellDimensions; uint nodeOffset;
    float3 boundsMaximum; uint cellOffset;
    uint referenceOffset; uint triangleOffset; uint triangleCount; uint padding;
};

struct VXGIMeshSDFInstance
{
    row_major float4x4 worldFromLocal;
    row_major float4x4 localFromWorld;
    row_major float4x4 normalFromLocal;
    float3 boundsMinimum; uint assetIndex;
    float3 boundsMaximum; uint materialOffset;
    uint materialCount; uint instanceId; uint flags; uint sceneInstanceCount;
};

StructuredBuffer<VXGIMeshSDFAsset> VXGIMeshSDFAssets : register(t0, space1);
StructuredBuffer<VXGIMeshSDFInstance> VXGIMeshSDFInstances : register(t1, space1);
StructuredBuffer<uint> VXGIMeshSDFMaterials : register(t2, space1);

#endif
