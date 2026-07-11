cbuffer PassConstants : register(b0)
{
    float3 camPos;
    float padding1;
    float3 lightDir;
    float padding2;
    float3 lightColor;
    float padding3;
    
    float4x4 lightViewProj[4];
    float4 cascadeSplits;
    
    uint iblPrefilterIdx;
    uint iblBRDFIdx;
    uint shadowMapIdx;
};

cbuffer MeshConstants : register(b1)
{
    uint materialID;
    uint cascadeIdx;
};

struct InstanceData
{
    float4x4 wvpMat;
    float4x4 worldMat;
    float4x4 normalMat;
    uint customMaterialID;
    uint3 pad;
};

struct MaterialData
{
    uint albedoIdx;
    uint normalIdx;
    uint ormIdx;
    uint emissiveIdx;
    float4 baseColorFactor;
    uint isUnlit;
    uint3 pad;
};

StructuredBuffer<InstanceData> gInstanceData : register(t6);
StructuredBuffer<MaterialData> gMaterialData : register(t7);
SamplerState s1 : register(s0);

#include "DepthVisibility.hlsli"

struct VS_INPUT
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 texCoord : TEXCOORD;
    float3 tangent : TANGENT;
    float3 bitangent : BITANGENT;
    int4 boneIds : BLENDINDICES;
    float4 weights : BLENDWEIGHT;
    uint instanceID : SV_InstanceID;
};

struct VS_OUTPUT
{
    float4 pos : SV_POSITION;
    float2 texCoord : TEXCOORD;
    nointerpolation uint instanceID : TEXCOORD1;
};

// Outputs the 4D position of each vertex from the light's perspective
// The rasterizer will later extract the depth (Z) from this to build the Shadow Map
VS_OUTPUT VSMain(VS_INPUT input)
{
    VS_OUTPUT output;
    float4x4 worldMat = gInstanceData[input.instanceID].worldMat;
    output.pos = mul(mul(float4(input.pos, 1.0f), worldMat), lightViewProj[cascadeIdx]);
    output.texCoord = input.texCoord;
    output.instanceID = input.instanceID;
    return output;
}

#ifdef ALPHA_TEST
void PSMain(VS_OUTPUT input)
{
    ClipByDepthVisibility(input.instanceID, materialID, input.texCoord);
}
#endif
