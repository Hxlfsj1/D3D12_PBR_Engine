#ifndef VXGI_MATERIAL_COMMON_HLSLI
#define VXGI_MATERIAL_COMMON_HLSLI

// Exactly the 48-byte global MaterialData/RenderMaterialData GPU layout.
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
};

#endif
