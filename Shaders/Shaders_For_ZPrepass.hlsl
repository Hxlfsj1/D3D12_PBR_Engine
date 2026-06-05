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
    uint isUnlit;
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
    nointerpolation uint instanceID : TEXCOORD1;
};

VS_OUTPUT VSMain(VS_INPUT input)
{
    VS_OUTPUT output;
    output.pos = mul(float4(input.pos, 1.0f), gInstanceData[input.instanceID].wvpMat);
    output.texCoord = input.texCoord;
    output.instanceID = input.instanceID;
    
    return output;
}

#ifdef ALPHA_TEST
void PSMain(VS_OUTPUT input)
{
    uint instMatID = gInstanceData[input.instanceID].customMaterialID;
    uint finalMatID = (instMatID != 0xFFFFFFFF) ? instMatID : materialID;
    
    Texture2D tAlbedo = ResourceDescriptorHeap[gMaterialData[finalMatID].albedoIdx];
    float alpha = tAlbedo.Sample(s1, input.texCoord).a;
    
    clip(alpha - 0.5f);
}
#endif