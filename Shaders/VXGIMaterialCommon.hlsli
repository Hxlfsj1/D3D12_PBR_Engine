#ifndef VXGI_MATERIAL_COMMON_HLSLI
#define VXGI_MATERIAL_COMMON_HLSLI

// Exactly the 80-byte global MaterialData/RenderMaterialData GPU layout.
// Shared by geometric coverage and surface shading without an include cycle.
struct VXGIMaterialData
{
    uint albedoIdx;
    uint normalIdx;
    uint ormIdx;
    uint emissiveIdx;
    float4 baseColorFactor;
    uint isUnlit;
    float3 emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
    uint occlusionIdx;
    float occlusionStrength;
    float alphaCutoff;
    float3 materialPadding;
};

#endif
