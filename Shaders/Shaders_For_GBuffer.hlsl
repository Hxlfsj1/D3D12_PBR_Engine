/*
G-Buffer Pass Output Summary:

1. Target0: BaseColor and Alpha
2. Target1: Normal in [-1, 1]
3. Target2: ORM texture
4. Target3: Emissive Color
*/

#ifndef LOD_LEVEL
#define LOD_LEVEL 0
#endif

struct MaterialData
{
    uint albedoIdx;
    uint normalIdx;
    uint ormIdx;
    uint emissiveIdx;
    uint isUnlit;
    uint3 pad;
};

struct InstanceData
{
    float4x4 wvpMat;
    float4x4 worldMat;
    float4x4 normalMat;
    
    uint customMaterialID;
    uint3 pad;
};

cbuffer MeshConstants : register(b1)
{
    uint materialID;
};

StructuredBuffer<InstanceData> gInstanceData : register(t6);
StructuredBuffer<MaterialData> gMaterialData : register(t7);
SamplerState s1 : register(s0);

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
    float3 worldNormal : NORMAL;
#if LOD_LEVEL == 0
    float3 worldTangent : TANGENT;
    float3 worldBitangent : BITANGENT;
#endif
    nointerpolation uint instanceID : TEXCOORD5;
};

VS_OUTPUT VSMain(VS_INPUT input)
{
    VS_OUTPUT output;
    output.pos = mul(float4(input.pos, 1.0f), gInstanceData[input.instanceID].wvpMat);
    output.texCoord = input.texCoord;
    float4x4 normalMat = gInstanceData[input.instanceID].normalMat;
    output.worldNormal = mul(input.normal, (float3x3) normalMat);
#if LOD_LEVEL == 0
    output.worldTangent = mul(input.tangent, (float3x3) normalMat);
    output.worldBitangent = mul(input.bitangent, (float3x3) normalMat);
#endif
    output.instanceID = input.instanceID;
    
    return output;
}

struct GBufferOutput
{
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
    float4 orm : SV_Target2;
    float4 emissive : SV_Target3;
};

GBufferOutput PSMain(VS_OUTPUT input)
{
    GBufferOutput output;
    
    uint instMatID = gInstanceData[input.instanceID].customMaterialID;
    uint finalMatID = (instMatID != 0xFFFFFFFF) ? instMatID : materialID;
    
    Texture2D tAlbedo = ResourceDescriptorHeap[gMaterialData[finalMatID].albedoIdx];
    float4 albedoSample = tAlbedo.Sample(s1, input.texCoord);
    
#ifdef ALPHA_TEST
    clip(albedoSample.a - 0.5f);
#endif
    
    float3 baseAlbedo = pow(abs(albedoSample.rgb), 2.2);
    output.albedo = float4(baseAlbedo, albedoSample.a);
    
    Texture2D tEmissive = ResourceDescriptorHeap[gMaterialData[finalMatID].emissiveIdx];
    float3 emissiveSample = tEmissive.Sample(s1, input.texCoord).rgb;

    if (gMaterialData[finalMatID].isUnlit != 0)
    {
        output.emissive = float4(baseAlbedo, 1.0f);
        output.albedo = float4(0.0f, 0.0f, 0.0f, albedoSample.a);
    }
    else
    {
        output.emissive = float4(emissiveSample, 1.0f);
    }
    
#if LOD_LEVEL == 0
    Texture2D tNormal = ResourceDescriptorHeap[gMaterialData[finalMatID].normalIdx];
    float3 normalMap = tNormal.Sample(s1, input.texCoord).xyz * 2.0 - 1.0;
    normalMap.y = -normalMap.y;
    float3 N = normalize(input.worldNormal);
    float3 T = normalize(input.worldTangent);
    float3 B = normalize(input.worldBitangent);
    float3x3 TBN = float3x3(T, B, N);
    float3 finalNormal = normalize(mul(normalMap, TBN));
    output.normal = float4(finalNormal * 0.5 + 0.5, 1.0);
#else
    float3 finalNormal = normalize(input.worldNormal);
    output.normal = float4(finalNormal * 0.5 + 0.5, 1.0);
#endif

    Texture2D tORM = ResourceDescriptorHeap[gMaterialData[finalMatID].ormIdx];
    float3 ormSample = tORM.Sample(s1, input.texCoord).rgb;
    output.orm = float4(ormSample, 1.0f);
    
    if (gMaterialData[finalMatID].isUnlit != 0)
    {
        output.orm.a = 0.0f;
    }
    else
    {
        output.orm.a = 1.0f;
    }
    
    return output;
}