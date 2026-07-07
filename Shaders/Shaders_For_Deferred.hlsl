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
    float3 N = normalize(tNormal.SampleLevel(s1, input.texCoord, 0).xyz * 2.0f - 1.0f);
    float4 ormSample = tORM.SampleLevel(s1, input.texCoord, 0);
    if (ormSample.a < 0.5f)
    {
        return float4(albedo, 1.0f);
    }
    float ao = max(ormSample.r, 0.01);
    float roughness = max(ormSample.g, 0.005);
    float metallic = ormSample.b;

    // Reconstruct world position from NDC coordinates and depth
    float x = input.texCoord.x * 2.0f - 1.0f;
    float y = 1.0f - input.texCoord.y * 2.0f;
    float4 clipSpacePos = float4(x, y, depth, 1.0f);
    float4 worldPosH = mul(clipSpacePos, invViewProj);
    float3 worldPos = worldPosH.xyz / worldPosH.w;

    // Use different shadow maps depending on distance
    float dist = distance(camPos, worldPos);
    uint cascadeIndex = 0;
    if (dist > cascadeSplits.x)
        cascadeIndex = 1;
    if (dist > cascadeSplits.y)
        cascadeIndex = 2;
    if (dist > cascadeSplits.z)
        cascadeIndex = 3;

    float4 lightSpacePos = mul(float4(worldPos, 1.0f), lightViewProj[cascadeIndex]);

    float3 V = normalize(camPos - worldPos);
    float3 R = reflect(-V, N);
    float3 F0 = float3(0.04, 0.04, 0.04);
    F0 = lerp(F0, albedo, metallic);
    float3 L = normalize(-lightDir);
    float3 H = normalize(V + L);
    float3 radiance = lightColor;

    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    float3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    
    float3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
    float3 specular = numerator / denominator;

    float3 kS = F;
    float3 kD = float3(1.0, 1.0, 1.0) - kS;
    kD *= 1.0 - metallic;

    // Shadow with attenuation
    float shadow = CalcShadowFactor(lightSpacePos, cascadeIndex);
    float fadeDistance = 10.0f;
    float fadeStart = cascadeSplits.w - fadeDistance;
    float fadeFactor = saturate((dist - fadeStart) / fadeDistance);
    shadow = lerp(shadow, 1.0f, fadeFactor);
    
    float NdotL = max(dot(N, L), 0.0);
    float3 directDiffuse = (kD * albedo / PI) * radiance * NdotL * shadow;
    float3 directSpecular = specular * radiance * NdotL * shadow;
    
    float3 Lo = directDiffuse + directSpecular;

    float3 F_IBL = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    float3 kS_IBL = F_IBL;
    float3 kD_IBL = 1.0 - kS_IBL;
    kD_IBL *= 1.0 - metallic;
    
    float3 irradiance = EvaluateSH9(N);
    float3 diffuse_IBL = irradiance * albedo;
    
    Texture2D tHBAO = ResourceDescriptorHeap[hbaoIdx];
    float hbao = tHBAO.SampleLevel(s1, input.texCoord, 0).r;

    float finalAO = min(ao, hbao);

    float3 ambientDiffuse = kD_IBL * diffuse_IBL * finalAO;

    TextureCube tPrefilter = ResourceDescriptorHeap[iblPrefilterIdx];
    float3 prefilteredColor = tPrefilter.SampleLevel(s1, R, roughness * 4.0).rgb;
    Texture2D tBRDF = ResourceDescriptorHeap[iblBRDFIdx];
    float2 brdf = tBRDF.Sample(s1, float2(max(dot(N, V), 0.0), roughness)).rg;
    float3 specular_IBL = prefilteredColor * (F_IBL * brdf.x + brdf.y);
    float3 ambientSpecular = specular_IBL * finalAO; 
    float3 ambient = ambientDiffuse + ambientSpecular;
    
    float3 PBR_Color = ambient + Lo;
    
    Texture2D tEmissive = ResourceDescriptorHeap[gbufferEmissiveIdx];
    float3 emissiveColor = tEmissive.SampleLevel(s1, input.texCoord, 0).rgb;
    
    float3 color = PBR_Color + emissiveColor;

    return float4(color, 1.0f);
}
