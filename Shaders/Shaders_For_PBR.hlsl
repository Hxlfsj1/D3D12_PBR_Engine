cbuffer PassConstants : register(b0)
{
    float3 camPos;
    float padding1;
    float3 lightPos;
    float padding2;
    float3 lightColor;
    float padding3;
    float4x4 lightViewProj;
};

Texture2D tAlbedo : register(t0);
Texture2D tNormal : register(t1);
Texture2D tMR : register(t2);
Texture2D tEmissive : register(t3);
TextureCube tPrefilter : register(t4);
Texture2D tBRDF : register(t5);
Texture2D tShadowMap : register(t7);

struct InstanceData
{
    float4x4 wvpMat;
    float4x4 worldMat;
    float4x4 normalMat;
};
StructuredBuffer<InstanceData> gInstanceData : register(t6);

SamplerState s1 : register(s0);
SamplerComparisonState shadowSampler : register(s1);

cbuffer MaterialFlags : register(b1)
{
    int hasAlbedo;
    int hasNormal;
    int hasORM;
    int hasEmissive;
};

cbuffer SHBuffer : register(b2)
{
    float3 SHCoefficients[9];
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
    float3 tangent : TANGENT;
    float3 bitangent : BITANGENT;
    float4 lightSpacePos : LIGHTSPACE;
};

static const float PI = 3.14159265359;

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
    
    // Completely fix the non-uniform scaling issue for normals by implementing a dedicated Normal Matrix
    output.normal = normalize(mul(input.normal, (float3x3) normalMat));
    output.tangent = normalize(mul(input.tangent, (float3x3) normalMat));
    output.bitangent = normalize(mul(input.bitangent, (float3x3) normalMat));
    output.texCoord = input.texCoord;
    
    output.lightSpacePos = mul(float4(output.worldPos, 1.0f), lightViewProj);
    
    return output;
}

float3 getNormalFromMap(VS_OUTPUT input)
{
    float3 tangentNormal = tNormal.Sample(s1, input.texCoord).xyz * 2.0 - 1.0;
    tangentNormal.y = -tangentNormal.y;

    float3 T = normalize(input.tangent);
    float3 B = normalize(input.bitangent);
    float3 N = normalize(input.normal);
    T = normalize(T - dot(T, N) * N);
    float3x3 TBN = float3x3(T, B, N);
    
    return normalize(mul(tangentNormal, TBN));
}

// SH
float3 EvaluateSH9(float3 N)
{
    float x = N.x;
    float y = N.y;
    float z = N.z;

    float3 result =
        SHCoefficients[0] +
        SHCoefficients[1] * y +
        SHCoefficients[2] * z +
        SHCoefficients[3] * x +
        SHCoefficients[4] * (x * y) +
        SHCoefficients[5] * (y * z) +
        SHCoefficients[6] * (3.0 * z * z - 1.0) +
        SHCoefficients[7] * (x * z) +
        SHCoefficients[8] * (x * x - y * y);

    return max(result, float3(0.0, 0.0, 0.0));
}

// NDF
float DistributionGGX(float3 N, float3 H, float roughness)
{   
    // Perceptual Remapping: Linearizing parameter control for intuitive artist adjustment.
    float a = roughness * roughness;
    float a2 = a * a;
    
    // Use max(0, dot) to prevent negative light contributions from back-facing sources
    float NdotH = max(dot(N, H), 0.0);
    
    // GGX formula
    float denom = (NdotH * NdotH * (a2 - 1.0) + 1.0);
    return a2 / max(PI * denom * denom, 0.0000001);
}

// Single-direction geometric shadowing/masking function
float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

// Geometric Shadowing/Masking Function
float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    return GeometrySchlickGGX(max(dot(N, V), 0.0), roughness) * GeometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

// Classic Schlick's Fresnel, approaching pure white at grazing angles, suitable for non-IBL lighting
float3 fresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Roughness-dependent Fresnel term: Attenuating specular intensity for IBL
float3 fresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    return F0 + (max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

static const float2 POISSON_DISK[16] =
{
    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725),
    float2(-0.094184101, -0.92938870), float2(0.34495938, 0.29387760),
    float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464),
    float2(-0.38277543, 0.27676845), float2(0.97484398, 0.75648379),
    float2(0.44323325, -0.97511554), float2(0.53742981, -0.47373420),
    float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
    float2(-0.24188840, 0.99706507), float2(-0.81409955, 0.91437590),
    float2(0.19984126, 0.78641367), float2(0.14383161, -0.14100467)
};

// Pseudo-random number generator (PRNG) for rotating the Poisson Disk
float Rand(float2 co)
{
    return frac(sin(dot(co.xy, float2(12.9898, 78.233))) * 43758.5453);
}

// Blocker search using Poisson Disk sampling
void FindBlocker(out float avgBlockerDepth, out float numBlockers, float2 uv, float zReceiver, float searchRadius)
{
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
        
        if (shadowMapDepth < zReceiver - 0.001)
        {
            blockerSum += shadowMapDepth;
            numBlockers += 1.0;
        }
    }
    avgBlockerDepth = numBlockers > 0.0 ? blockerSum / numBlockers : 1.0;
}

float CalcShadowFactor(float4 lightSpacePos)
{
    float3 projCoords = lightSpacePos.xyz / lightSpacePos.w;
    projCoords.x = projCoords.x * 0.5f + 0.5f;
    projCoords.y = -projCoords.y * 0.5f + 0.5f;
    
    if (projCoords.z > 1.0f || projCoords.x < 0.0f || projCoords.x > 1.0f || projCoords.y < 0.0f || projCoords.y > 1.0f)
        return 1.0f;

    float zReceiver = projCoords.z;
    
    // Adjustable shadow parameters
    float LIGHT_WORLD_SIZE = 1.0;
    float LIGHT_FRUSTUM_WIDTH = 50.0;
    float LIGHT_SIZE_UV = LIGHT_WORLD_SIZE / LIGHT_FRUSTUM_WIDTH;
    
    // First, perform a blocker search using Poisson Disk sampling
    float avgBlockerDepth = 1.0;
    float numBlockers = 0.0;
    float searchRadius = LIGHT_SIZE_UV * 0.5;
    FindBlocker(avgBlockerDepth, numBlockers, projCoords.xy, zReceiver, searchRadius);
    
    if (numBlockers < 1.0)
        return 1.0f;

    // Next, scale the Poisson Disk radius using similar triangles to achieve a variable penumbra
    float penumbraRatio = (zReceiver - avgBlockerDepth) / max(avgBlockerDepth, 0.0001);
    float filterRadius = penumbraRatio * LIGHT_SIZE_UV;

    // Finally, perform the shadow calculation using the variable-radius Poisson Disk
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

// PBR pixel Shader
float4 PSMain(VS_OUTPUT input) : SV_TARGET
{
    float3 albedo = pow(tAlbedo.Sample(s1, input.texCoord).rgb, 2.2);
    float ao = 1.0;
    float roughness = 0.5;
    float metallic = 0.0;
    
    float4 mrSample = tMR.Sample(s1, input.texCoord);
    ao = max(mrSample.r, 0.01);
    roughness = mrSample.g;
    metallic = mrSample.b;
    
    float3 N = getNormalFromMap(input);
    float3 V = normalize(camPos - input.worldPos);
    float3 R = reflect(-V, N);
    float3 F0 = float3(0.04, 0.04, 0.04);
    F0 = lerp(F0, albedo, metallic);
    float3 L = normalize(lightPos - input.worldPos);
    float3 H = normalize(V + L);
    float3 radiance = lightColor;

    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    float3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    // Construct the classic Cook-Torrance reflectance equation
    float3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
    float3 specular = numerator / denominator;

    // kD = (1 - F) * (1 - metallic)
    float3 kS = F;
    float3 kD = float3(1.0, 1.0, 1.0) - kS;
    kD *= 1.0 - metallic;

    float shadow = CalcShadowFactor(input.lightSpacePos);
    
    // Calculate final light
    float NdotL = max(dot(N, L), 0.0);
    float3 Lo = (kD * albedo / PI + specular) * radiance * NdotL * shadow;
    
    // Diffuse IBL
    float3 F_IBL = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    float3 kS_IBL = F_IBL;
    float3 kD_IBL = 1.0 - kS_IBL;
    kD_IBL *= 1.0 - metallic;
    float3 irradiance = EvaluateSH9(N);
    float3 diffuse_IBL = irradiance * albedo;
    
    // Specular IBL (split sum)
    const float MAX_REFLECTION_LOD = 4.0;
    float3 prefilteredColor = tPrefilter.SampleLevel(s1, R, roughness * MAX_REFLECTION_LOD).rgb;
    float2 brdf = tBRDF.Sample(s1, float2(max(dot(N, V), 0.0), roughness)).rg;
    float3 specular_IBL = prefilteredColor * (F0 * brdf.x + brdf.y);
    
    float3 ambient = (kD_IBL * diffuse_IBL + specular_IBL) * ao;
    // Add emissive (if applicable)
    float3 emissive = hasEmissive ? pow(tEmissive.Sample(s1, input.texCoord).rgb, 2.2) : float3(0.0, 0.0, 0.0);
    float3 color = ambient + Lo + emissive;

    // ACES Filmic Tone Mapping
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    color = saturate((color * (a * color + b)) / (color * (c * color + d) + e));
    
    color = pow(color, 1.0 / 2.2);

    return float4(color, 1.0);
}