#ifndef LOD_LEVEL
#define LOD_LEVEL 0
#endif

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
    float4 cascadeOrthoWidths;
    
    uint iblPrefilterIdx;
    uint iblBRDFIdx;
    uint shadowMapIdx;
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
    uint transparentSceneColorIdx;
};

StructuredBuffer<InstanceData> gInstanceData : register(t6);

SamplerState s1 : register(s0);
SamplerComparisonState shadowSampler : register(s1);

StructuredBuffer<MaterialData> gMaterialData : register(t7);

#include "DepthVisibility.hlsli"
#include "MaterialCommon.hlsli"
#include "TangentBasis.hlsli"

#define hasAlbedo 1
#define hasNormal 1
#define hasORM 1
#define hasEmissive 1

cbuffer SHBuffer : register(b2)
{
    float4 SHCoefficients[9];
};

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
    float3 worldPos : POSITION;
    float3 normal : NORMAL;
    float2 texCoord : TEXCOORD;

#if LOD_LEVEL == 0
    float3 tangent : TANGENT;
    float3 bitangent : BITANGENT;
#endif
    nointerpolation uint instanceID : TEXCOORD5;
};

// PBR vertex shader
VS_OUTPUT VSMain(VS_INPUT input)
{
    VS_OUTPUT output;
    
    // Get model's matrix by its ID
    float4x4 wvpMat = gInstanceData[input.instanceID].wvpMat;
    float4x4 worldMat = gInstanceData[input.instanceID].worldMat;
    float4x4 normalMat = gInstanceData[input.instanceID].normalMat;

    output.pos = mul(float4(input.pos, 1.0f), wvpMat);
    output.worldPos = mul(float4(input.pos, 1.0f), worldMat).xyz;
    
    output.normal = normalize(mul(input.normal, (float3x3) normalMat));
    output.texCoord = input.texCoord;
    
#if LOD_LEVEL == 0
    // Completely fix the non-uniform scaling issue for normals by implementing a dedicated Normal Matrix
    output.tangent = normalize(mul(input.tangent, (float3x3) normalMat));
    output.bitangent = normalize(mul(input.bitangent, (float3x3) normalMat));
#endif
    
    output.instanceID = input.instanceID;
    
    return output;
}

#if LOD_LEVEL == 0
float3 getNormalFromMap(VS_OUTPUT input, bool isFrontFace)
{
    uint finalMatID = ResolveDepthMaterialID(input.instanceID, materialID);

    uint normalIdx = gMaterialData[finalMatID].normalIdx;
    Texture2D tNormal = ResourceDescriptorHeap[normalIdx];
    float3 tangentNormal = tNormal.Sample(s1, input.texCoord).xyz * 2.0 - 1.0;
    tangentNormal.y = -tangentNormal.y;

    float3 N;
    float3 T;
    float3 B;
    BuildOrthonormalTangentBasis(input.normal, input.tangent, input.bitangent, N, T, B);
    float3x3 TBN = float3x3(T, B, N);
    float3 mappedNormal = normalize(mul(tangentNormal, TBN));

    return FaceNormalByFrontFace(mappedNormal, isFrontFace);
}
#endif

#include "LightingCommon.hlsli"

static const float TRANSPARENT_FRESNEL_POWER = 5.0f;
static const float TRANSPARENT_FRESNEL_DARKEN_STRENGTH = 0.35f;

static const float TRANSPARENT_REFRACTION_STRENGTH = 0.015f;
static const float TRANSPARENT_REFRACTION_SURFACE_WEIGHT = 0.35f;

float3 ApplyTransparentFresnelDarkening(float3 color, float3 normal, float3 viewDir)
{
#ifdef TRANSPARENT_PASS
    float ndotv = saturate(dot(normalize(normal), normalize(viewDir)));
    float edge = pow(1.0f - ndotv, TRANSPARENT_FRESNEL_POWER);
    float transmittance = lerp(1.0f, 1.0f - TRANSPARENT_FRESNEL_DARKEN_STRENGTH, edge);
    return color * transmittance;
#else
    return color;
#endif
}

float3 ApplyTransparentSceneRefraction(float3 surfaceColor, float alpha, float4 screenPos, float3 normal, float3 albedo)
{
#ifdef TRANSPARENT_PASS
    Texture2D sceneColorTexture = ResourceDescriptorHeap[transparentSceneColorIdx];

    uint sceneWidth;
    uint sceneHeight;
    sceneColorTexture.GetDimensions(sceneWidth, sceneHeight);

    float2 sceneUv = screenPos.xy / float2(sceneWidth, sceneHeight);
    float2 refractedUv = saturate(sceneUv + normal.xy * TRANSPARENT_REFRACTION_STRENGTH);
    float3 refractedColor = sceneColorTexture.Sample(s1, refractedUv).rgb;

    float surfaceWeight = saturate(max(alpha, TRANSPARENT_REFRACTION_SURFACE_WEIGHT));
    return lerp(refractedColor * albedo, surfaceColor, surfaceWeight);
#else
    return surfaceColor;
#endif
}

// PBR pixel Shader
float4 PSMain(VS_OUTPUT input, bool isFrontFace : SV_IsFrontFace) : SV_TARGET
{
    uint finalMatID = ResolveDepthMaterialID(input.instanceID, materialID);
    
    MaterialData mat = gMaterialData[finalMatID];
    
    float4 albedoSample = SampleDepthAlbedo(finalMatID, input.texCoord);
    ApplyDepthAlphaTest(albedoSample.a);
    float3 albedo = DecodeSRGBColor(albedoSample.rgb) * mat.baseColorFactor.rgb;
    float finalAlpha = albedoSample.a;
    
    if (mat.isUnlit)
    {
        float3 N = FaceNormalByFrontFace(normalize(input.normal), isFrontFace);
        float3 V = normalize(camPos - input.worldPos);
        float3 transparentColor = ApplyTransparentSceneRefraction(albedo, finalAlpha, input.pos, N, albedo);
        transparentColor = ApplyTransparentFresnelDarkening(transparentColor, N, V);
        return BuildSurfaceOutput(transparentColor, finalAlpha);
    }

#if LOD_LEVEL == 2
    float3 N = normalize(input.normal);
    N = FaceNormalByFrontFace(N, isFrontFace);
    float3 L = normalize(-lightDir);
    float NdotL = max(dot(N, L), 0.0);
    
    float3 irradiance = EvaluateSH9(N);
    float3 diffuse_IBL = irradiance * albedo;
    float3 directDiffuse = albedo * lightColor * NdotL;
    
    float3 totalDiffuse = diffuse_IBL + directDiffuse;
    float3 V = normalize(camPos - input.worldPos);
    totalDiffuse = ApplyTransparentSceneRefraction(totalDiffuse, finalAlpha, input.pos, N, albedo);
    totalDiffuse = ApplyTransparentFresnelDarkening(totalDiffuse, N, V);
    return BuildSurfaceOutput(totalDiffuse, finalAlpha);
#else
    Texture2D tMR = ResourceDescriptorHeap[mat.ormIdx];
    
    float ao = 1.0;
    float roughness = 0.5;
    float metallic = 0.0;
    
    float4 mrSample = tMR.Sample(s1, input.texCoord);
    ao = max(mrSample.r, 0.01);
    roughness = ClampPerceptualRoughness(mrSample.g);
    metallic = mrSample.b;
    
    float3 N;
    
#if LOD_LEVEL == 1
    N = normalize(input.normal);
    N = FaceNormalByFrontFace(N, isFrontFace);
#else
    N = getNormalFromMap(input, isFrontFace);
#endif

    float3 V = normalize(camPos - input.worldPos);
    float3 R = reflect(-V, N);
    float3 F0 = ComputeMaterialF0(albedo, metallic);
    float3 L = normalize(-lightDir);
    float3 H = normalize(V + L);
    float3 radiance = lightColor;

    float3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    float3 specular = ComputeCookTorranceSpecular(N, V, L, F, roughness);

    float3 kD = ComputeDiffuseEnergy(F, metallic);

    // Use different shadow maps depending on distance
    float dist = distance(camPos, input.worldPos);
    uint cascadeIndex = SelectCascadeIndex(dist);

    float4 lightSpacePos = mul(float4(input.worldPos, 1.0f), lightViewProj[cascadeIndex]);
    float shadow = CalcShadowFactor(lightSpacePos, cascadeIndex);
    shadow = FadeCascadeShadow(shadow, dist);
    
    // Calculate final light
    float NdotL = max(dot(N, L), 0.0);
    float3 directDiffuse = ComputeDirectDiffuse(kD, albedo, radiance, NdotL, shadow);
    float3 directSpecular = ComputeDirectSpecular(specular, radiance, NdotL, shadow);
    
    // Diffuse IBL (SH)
    float3 F_IBL = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    float3 kD_IBL = ComputeDiffuseEnergy(F_IBL, metallic);
    float3 irradiance = EvaluateSH9(N);
    float3 diffuse_IBL = irradiance * albedo;
    float3 specular_IBL = float3(0.0, 0.0, 0.0);
    
#if LOD_LEVEL == 0
    // Specular IBL (split sum)
    TextureCube tPrefilter = ResourceDescriptorHeap[iblPrefilterIdx];
    Texture2D tBRDF = ResourceDescriptorHeap[iblBRDFIdx];
    
    float3 prefilteredColor = tPrefilter.SampleLevel(s1, R, roughness * SPECULAR_IBL_MAX_MIP).rgb;
    float2 brdf = tBRDF.Sample(s1, float2(max(dot(N, V), 0.0), roughness)).rg;
    specular_IBL = ComputeSplitSumSpecularIBL(prefilteredColor, brdf, F_IBL);
#elif LOD_LEVEL == 1
    TextureCube tPrefilter = ResourceDescriptorHeap[iblPrefilterIdx];

    float3 prefilteredColor = tPrefilter.SampleLevel(s1, R, roughness * SPECULAR_IBL_MAX_MIP).rgb;
    specular_IBL = ComputeSimpleSpecularIBL(prefilteredColor, F_IBL);
#endif
    
    // The kD_IBL term is decoupled from the split-sum specular BRDF
    // It serves as a visual constraint to mimic energy conservation rather than achieving strict physical correctness
    float3 ambientDiffuse = kD_IBL * diffuse_IBL * ao;
    float3 ambientSpecular = specular_IBL * ao;
    
    // Add emissive (if applicable)
    Texture2D tEmissive = ResourceDescriptorHeap[mat.emissiveIdx];
    float3 emissive = hasEmissive ? DecodeSRGBColor(tEmissive.Sample(s1, input.texCoord).rgb) : float3(0.0, 0.0, 0.0);
    
    float3 totalDiffuse = directDiffuse + ambientDiffuse;
    float3 totalSpecular = directSpecular + ambientSpecular;
    
    float3 finalColor = totalDiffuse + totalSpecular + emissive;
    finalColor = ApplyTransparentSceneRefraction(finalColor, finalAlpha, input.pos, N, albedo);
    finalColor = ApplyTransparentFresnelDarkening(finalColor, N, V);

    return BuildSurfaceOutput(finalColor, finalAlpha);
#endif
}
