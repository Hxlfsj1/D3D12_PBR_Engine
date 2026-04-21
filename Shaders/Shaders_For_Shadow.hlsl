cbuffer PassConstants : register(b0)
{
    float3 camPos;
    float padding1;
    float3 lightDir;
    float padding2;
    float3 lightColor;
    float padding3;
    float4x4 lightViewProj;
};

struct InstanceData
{
    float4x4 wvpMat;
    float4x4 worldMat;
    float4x4 normalMat;
};
StructuredBuffer<InstanceData> gInstanceData : register(t0);

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

float4 VSMain(VS_INPUT input) : SV_POSITION
{
    float4x4 worldMat = gInstanceData[input.instanceID].worldMat;
    float4 worldPos = mul(float4(input.pos, 1.0f), worldMat);
    return mul(worldPos, lightViewProj);
}