cbuffer PassConstants : register(b0)
{
    float3 camPos;
    float padding1;
    float3 lightDir;
    float padding2;
    float3 lightColor;
    float tanSunAngularRadius;
    
    float4x4 lightViewProj[4];
    float4 cascadeSplits;
    float4 cascadeOrthoWidths;
    float4 cascadeDepthRanges;
    
    uint iblPrefilterIdx;
    uint iblBRDFIdx;
    uint shadowMapIdx;
};

cbuffer DeferredConstants : register(b1)
{
    float4x4 invViewProj;
    uint gbufferAlbedoIdx;
    uint gbufferNormalIdx;
    uint gbufferORMIdx;
    uint depthBufferIdx;
    uint gbufferEmissiveIdx;
    uint hbaoIdx;
};

cbuffer SHBuffer : register(b2)
{
    float4 SHCoefficients[9];
};

SamplerState s1 : register(s0);
SamplerComparisonState shadowSampler : register(s1);
SamplerState shadowDepthPointSampler : register(s2);

struct VS_OUTPUT
{
    float4 pos : SV_POSITION;
    float2 texCoord : TEXCOORD;
};

#include "FullscreenTriangle.hlsli"

// Draw a bufferless fullscreen triangle
VS_OUTPUT VSMain(uint vertexID : SV_VertexID)
{
    VS_OUTPUT output;
    output.texCoord = GetFullscreenTriangleTexCoord(vertexID);
    output.pos = GetFullscreenTrianglePosition(output.texCoord);
    return output;
}

#include "LightingCommon.hlsli"

float4 PSMain(VS_OUTPUT input) : SV_TARGET
{
    Texture2D tAlbedo = ResourceDescriptorHeap[gbufferAlbedoIdx];
    Texture2D tNormal = ResourceDescriptorHeap[gbufferNormalIdx];
    Texture2D tORM = ResourceDescriptorHeap[gbufferORMIdx];
    Texture2D tDepth = ResourceDescriptorHeap[depthBufferIdx];

    // Cull the skybox area with a depth of 1 to skip shading
    float depth = tDepth.SampleLevel(s1, input.texCoord, 0).r;
    if (depth >= 1.0f)
    {
        discard;
    }

    // Unpack G-Buffer data
    float3 albedo = tAlbedo.SampleLevel(s1, input.texCoord, 0).rgb;
    float3 N = normalize(DecodeGBufferNormal(tNormal.SampleLevel(s1, input.texCoord, 0).xyz));
    float4 ormSample = tORM.SampleLevel(s1, input.texCoord, 0);
    if (ormSample.a < 0.5f)
    {
        return float4(albedo, 1.0f);
    }
    float ao = max(ormSample.r, 0.01);
    float roughness = ClampPerceptualRoughness(ormSample.g);
    float metallic = ormSample.b;

    // Reconstruct world position from NDC coordinates and depth
    float x = input.texCoord.x * 2.0f - 1.0f;
    float y = 1.0f - input.texCoord.y * 2.0f;
    float4 clipSpacePos = float4(x, y, depth, 1.0f);
    float4 worldPosH = mul(clipSpacePos, invViewProj);
    float3 worldPos = worldPosH.xyz / worldPosH.w;

    // Use different shadow maps depending on distance
    float dist = distance(camPos, worldPos);
    uint cascadeIndex = SelectCascadeIndex(dist);

    float4 lightSpacePos = mul(float4(worldPos, 1.0f), lightViewProj[cascadeIndex]);
    float4 lightSpacePosDDX = mul(float4(ddx(worldPos), 0.0f), lightViewProj[cascadeIndex]);
    float4 lightSpacePosDDY = mul(float4(ddy(worldPos), 0.0f), lightViewProj[cascadeIndex]);

    float3 V = normalize(camPos - worldPos);
    float3 R = reflect(-V, N);
    float3 F0 = ComputeMaterialF0(albedo, metallic);
    float3 L = normalize(-lightDir);
    float3 H = normalize(V + L);
    float3 radiance = lightColor;

    float3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    float3 specular = ComputeCookTorranceSpecular(N, V, L, F, roughness);

    float3 kD = ComputeDiffuseEnergy(F, metallic);

    // Shadow with attenuation
    float shadow = CalcShadowFactor(
        lightSpacePos,
        lightSpacePosDDX,
        lightSpacePosDDY,
        cascadeIndex);
    shadow = FadeCascadeShadow(shadow, dist);
    
    float NdotL = max(dot(N, L), 0.0);
    float3 directDiffuse = ComputeDirectDiffuse(kD, albedo, radiance, NdotL, shadow);
    float3 directSpecular = ComputeDirectSpecular(specular, radiance, NdotL, shadow);
    
    float3 Lo = directDiffuse + directSpecular;

    float3 F_IBL = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    float3 kD_IBL = ComputeDiffuseEnergy(F_IBL, metallic);
    
    float3 irradiance = EvaluateSH9(N);
    float3 diffuse_IBL = irradiance * albedo;
    
    Texture2D tHBAO = ResourceDescriptorHeap[hbaoIdx];
    float hbao = tHBAO.SampleLevel(s1, input.texCoord, 0).r;

    float finalAO = min(ao, hbao);

    float3 ambientDiffuse = kD_IBL * diffuse_IBL * finalAO;

    TextureCube tPrefilter = ResourceDescriptorHeap[iblPrefilterIdx];
    float3 prefilteredColor = tPrefilter.SampleLevel(s1, R, roughness * SPECULAR_IBL_MAX_MIP).rgb;
    Texture2D tBRDF = ResourceDescriptorHeap[iblBRDFIdx];
    float2 brdf = tBRDF.Sample(s1, float2(max(dot(N, V), 0.0), roughness)).rg;
    float3 specular_IBL = ComputeSplitSumSpecularIBL(prefilteredColor, brdf, F_IBL);
    float3 ambientSpecular = specular_IBL * finalAO; 
    float3 ambient = ambientDiffuse + ambientSpecular;
    
    float3 PBR_Color = ambient + Lo;
    
    Texture2D tEmissive = ResourceDescriptorHeap[gbufferEmissiveIdx];
    float3 emissiveColor = tEmissive.SampleLevel(s1, input.texCoord, 0).rgb;
    
    float3 color = PBR_Color + emissiveColor;

    return float4(color, 1.0f);
}
