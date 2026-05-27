cbuffer PassConstants : register(b0)
{
    float3 camPos;
    float padding1;
    float3 lightDir;
    float padding2;
    float3 lightColor;
    float padding3;
    float4x4 lightViewProj;
    
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
};

cbuffer SHBuffer : register(b2)
{
    float4 SHCoefficients[9];
};

SamplerState s1 : register(s0);
SamplerComparisonState shadowSampler : register(s1);

static const float PI = 3.14159265359;

struct VS_OUTPUT
{
    float4 pos : SV_POSITION;
    float2 texCoord : TEXCOORD;
};

VS_OUTPUT VSMain(uint vertexID : SV_VertexID)
{
    VS_OUTPUT output;
    output.texCoord = float2((vertexID << 1) & 2, vertexID & 2);
    output.pos = float4(output.texCoord * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

float3 EvaluateSH9(float3 N)
{
    float x = N.x, y = N.y, z = N.z;
    float3 result =
        SHCoefficients[0].xyz + SHCoefficients[1].xyz * y + SHCoefficients[2].xyz * z +
        SHCoefficients[3].xyz * x + SHCoefficients[4].xyz * (x * y) + SHCoefficients[5].xyz * (y * z) +
        SHCoefficients[6].xyz * (3.0 * z * z - 1.0) + SHCoefficients[7].xyz * (x * z) +
        SHCoefficients[8].xyz * (x * x - y * y);
    return max(result, float3(0.0, 0.0, 0.0));
}

float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float denom = (NdotH * NdotH * (a2 - 1.0) + 1.0);
    return a2 / max(PI * denom * denom, 0.0000001);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    return GeometrySchlickGGX(max(dot(N, V), 0.0), roughness) * GeometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

float3 fresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float3 fresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    return F0 + (max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

static const float2 POISSON_DISK[16] =
{
    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725), float2(-0.094184101, -0.92938870), float2(0.34495938, 0.29387760),
    float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464), float2(-0.38277543, 0.27676845), float2(0.97484398, 0.75648379),
    float2(0.44323325, -0.97511554), float2(0.53742981, -0.47373420), float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
    float2(-0.24188840, 0.99706507), float2(-0.81409955, 0.91437590), float2(0.19984126, 0.78641367), float2(0.14383161, -0.14100467)
};

float Rand(float2 co)
{
    return frac(sin(dot(co.xy, float2(12.9898, 78.233))) * 43758.5453);
}

void FindBlocker(out float avgBlockerDepth, out float numBlockers, float2 uv, float zReceiver, float searchRadius)
{
    Texture2D tShadowMap = ResourceDescriptorHeap[shadowMapIdx];
    float blockerSum = 0.0;
    numBlockers = 0.0;
    float randomAngle = Rand(uv) * 2.0 * PI;
    float cosTheta = cos(randomAngle);
    float sinTheta = sin(randomAngle);
    float2x2 rotMat = float2x2(cosTheta, -sinTheta, sinTheta, cosTheta);

    for (int i = 0; i < 16; ++i)
    {
        float2 offset = mul(POISSON_DISK[i], rotMat) * searchRadius;
        float shadowMapDepth = tShadowMap.SampleLevel(s1, uv + offset, 0).r;
        if (shadowMapDepth < zReceiver)
        {
            blockerSum += shadowMapDepth;
            numBlockers += 1.0;
        }
    }
    avgBlockerDepth = numBlockers > 0.0 ? blockerSum / numBlockers : 1.0;
}

float CalcShadowFactor(float4 lightSpacePos)
{
    Texture2D tShadowMap = ResourceDescriptorHeap[shadowMapIdx];
    float3 projCoords = lightSpacePos.xyz / lightSpacePos.w;
    projCoords.x = projCoords.x * 0.5f + 0.5f;
    projCoords.y = -projCoords.y * 0.5f + 0.5f;
    
    if (projCoords.z > 1.0f || projCoords.x < 0.0f || projCoords.x > 1.0f || projCoords.y < 0.0f || projCoords.y > 1.0f)
        return 1.0f;

    float zReceiver = projCoords.z;
    float LIGHT_WORLD_SIZE = 1.0;
    float LIGHT_SIZE_UV = LIGHT_WORLD_SIZE / padding3;
    
    float avgBlockerDepth = 1.0;
    float numBlockers = 0.0;
    float searchRadius = LIGHT_SIZE_UV * 0.5;
    FindBlocker(avgBlockerDepth, numBlockers, projCoords.xy, zReceiver, searchRadius);
    
    if (numBlockers < 1.0)
        return 1.0f;

    float penumbraRatio = (zReceiver - avgBlockerDepth) / max(avgBlockerDepth, 0.0001);
    float filterRadius = penumbraRatio * LIGHT_SIZE_UV;
    float shadow = 0.0f;
    float randomAngle = Rand(projCoords.xy + float2(1.0, 1.0)) * 2.0 * PI;
    float cosTheta = cos(randomAngle);
    float sinTheta = sin(randomAngle);
    float2x2 rotMat = float2x2(cosTheta, -sinTheta, sinTheta, cosTheta);

    for (int i = 0; i < 16; ++i)
    {
        float2 offset = mul(POISSON_DISK[i], rotMat) * filterRadius;
        shadow += tShadowMap.SampleCmpLevelZero(shadowSampler, projCoords.xy + offset, zReceiver).r;
    }
    return shadow / 16.0f;
}

float4 PSMain(VS_OUTPUT input) : SV_TARGET
{
    Texture2D tAlbedo = ResourceDescriptorHeap[gbufferAlbedoIdx];
    Texture2D tNormal = ResourceDescriptorHeap[gbufferNormalIdx];
    Texture2D tORM = ResourceDescriptorHeap[gbufferORMIdx];
    Texture2D tDepth = ResourceDescriptorHeap[depthBufferIdx];

    float depth = tDepth.SampleLevel(s1, input.texCoord, 0).r;
    if (depth >= 1.0f)
    {
        discard;
    }

    float3 albedo = tAlbedo.SampleLevel(s1, input.texCoord, 0).rgb;
    float3 N = normalize(tNormal.SampleLevel(s1, input.texCoord, 0).xyz);
    float4 ormSample = tORM.SampleLevel(s1, input.texCoord, 0);
    float ao = max(ormSample.r, 0.01);
    float roughness = max(ormSample.g, 0.04);
    float metallic = ormSample.b;

    float x = input.texCoord.x * 2.0f - 1.0f;
    float y = 1.0f - input.texCoord.y * 2.0f;
    float4 clipSpacePos = float4(x, y, depth, 1.0f);
    float4 worldPosH = mul(clipSpacePos, invViewProj);
    float3 worldPos = worldPosH.xyz / worldPosH.w;

    float4 lightSpacePos = mul(float4(worldPos, 1.0f), lightViewProj);

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

    float shadow = CalcShadowFactor(lightSpacePos);
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
    
    TextureCube tPrefilter = ResourceDescriptorHeap[iblPrefilterIdx];
    Texture2D tBRDF = ResourceDescriptorHeap[iblBRDFIdx];
    
    const float MAX_REFLECTION_LOD = 4.0;
    float3 prefilteredColor = tPrefilter.SampleLevel(s1, R, roughness * MAX_REFLECTION_LOD).rgb;
    float2 brdf = tBRDF.SampleLevel(s1, float2(max(dot(N, V), 0.0), roughness), 0).rg;
    float3 specular_IBL = prefilteredColor * (F0 * brdf.x + brdf.y);
    
    float3 ambient = (kD_IBL * diffuse_IBL + specular_IBL) * ao;
    
    float3 color = ambient + Lo;

    return float4(color, 1.0f);
}