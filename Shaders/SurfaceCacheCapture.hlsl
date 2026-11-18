#include "VXGIMaterialCommon.hlsli"
#include "MaterialCommon.hlsli"
#include "TangentBasis.hlsli"

// Card camera in model space. +Z points toward the orthographic camera;
// right/up/-Z is a left-handed camera frame with the engine's usual winding.
cbuffer CaptureConstants : register(b0)
{
    float3 cardOrigin; float capturePadding;
    float3 cardRight; float inverseExtentX;
    float3 cardUp; float inverseExtentY;
    float3 cardNormal; float inverseDepthSpan;
    row_major float4x4 worldFromLocal;
    row_major float4x4 normalFromLocal;
};
cbuffer CaptureDraw : register(b1) { uint materialId; uint alphaTest; };
StructuredBuffer<VXGIMaterialData> captureMaterials : register(t0);
SamplerState materialSampler : register(s0);

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
    float3 tangent : TANGENT;
    float3 bitangent : BITANGENT;
};
struct VertexOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float3 bitangent : BITANGENT;
};
VertexOutput VSCapture(VertexInput input)
{
    VertexOutput output;
    float3 p=input.position-cardOrigin;
    output.position=float4(dot(p,cardRight)*inverseExtentX,dot(p,cardUp)*inverseExtentY,
        0.5-dot(p,cardNormal)*inverseDepthSpan,1);
    output.uv=input.uv;
    output.normal=mul(input.normal,(float3x3)normalFromLocal);
    output.tangent=mul(input.tangent,(float3x3)worldFromLocal);
    output.bitangent=mul(input.bitangent,(float3x3)worldFromLocal);
    return output;
}
struct CaptureOutput
{
    float4 albedoMetallic : SV_Target0;
    float4 normalRoughness : SV_Target1;
    float4 emissiveSource : SV_Target2;
};
CaptureOutput PSCapture(VertexOutput input)
{
    VXGIMaterialData material=captureMaterials[materialId];
    Texture2D<float4> albedoTexture=ResourceDescriptorHeap[material.albedoIdx];
    float4 albedo=albedoTexture.Sample(materialSampler,input.uv)*material.baseColorFactor;
    if(alphaTest!=0)clip(albedo.a-material.alphaCutoff);
    Texture2D<float4> ormTexture=ResourceDescriptorHeap[material.ormIdx];
    float3 orm=ApplyMaterialORMFactors(ormTexture.Sample(materialSampler,input.uv).rgb,
        material.metallicFactor,material.roughnessFactor);
    float3 normal=normalize(input.normal);
    if(material.normalIdx!=NO_NORMAL_TEXTURE_INDEX)
    {
        Texture2D<float4> normalTexture=ResourceDescriptorHeap[material.normalIdx];
        float3 tangentNormal=normalTexture.Sample(materialSampler,input.uv).xyz*2-1;
        tangentNormal.y=-tangentNormal.y;
        float3 N,T,B;
        BuildOrthonormalTangentBasis(input.normal,input.tangent,input.bitangent,N,T,B);
        normal=normalize(mul(tangentNormal,float3x3(T,B,N)));
    }
    float3 emissive=0;
    if(material.isUnlit==0)
    {
        Texture2D<float4> emissiveTexture=ResourceDescriptorHeap[material.emissiveIdx];
        emissive=emissiveTexture.Sample(materialSampler,input.uv).rgb*material.emissiveFactor;
    }
    CaptureOutput output;
    output.albedoMetallic=float4(albedo.rgb,orm.b);
    output.normalRoughness=float4(normal,orm.g);
    output.emissiveSource=float4(emissive,material.isUnlit==0?1:0);
    return output;
}
