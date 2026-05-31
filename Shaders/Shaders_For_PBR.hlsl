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
    float4x4 lightViewProj;
    
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

SamplerState s1 : register(s0);
SamplerComparisonState shadowSampler : register(s1);

StructuredBuffer<MaterialData> gMaterialData : register(t7);

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

#if LOD_LEVEL < 2
    float4 lightSpacePos : LIGHTSPACE;
#endif
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
    
    output.normal = normalize(mul(input.normal, (float3x3) normalMat));
    output.texCoord = input.texCoord;
    
#if LOD_LEVEL == 0
    // Completely fix the non-uniform scaling issue for normals by implementing a dedicated Normal Matrix
    output.tangent = normalize(mul(input.tangent, (float3x3) normalMat));
    output.bitangent = normalize(mul(input.bitangent, (float3x3) normalMat));
#endif

#if LOD_LEVEL < 2
    output.lightSpacePos = mul(float4(output.worldPos, 1.0f), lightViewProj);
#endif
    output.instanceID = input.instanceID;
    
    return output;
}

#if LOD_LEVEL == 0
float3 getNormalFromMap(VS_OUTPUT input)
{
    uint instMatID = gInstanceData[input.instanceID].customMaterialID;
    uint finalMatID = (instMatID != 0xFFFFFFFF) ? instMatID : materialID;

    uint normalIdx = gMaterialData[finalMatID].normalIdx;
    Texture2D tNormal = ResourceDescriptorHeap[normalIdx];
    float3 tangentNormal = tNormal.Sample(s1, input.texCoord).xyz * 2.0 - 1.0;
    tangentNormal.y = -tangentNormal.y;

    float3 T = normalize(input.tangent);
    float3 B = normalize(input.bitangent);
    float3 N = normalize(input.normal);
    T = normalize(T - dot(T, N) * N);
    float3x3 TBN = float3x3(T, B, N);
    
    return normalize(mul(tangentNormal, TBN));
}
#endif

// Evaluate spherical harmonics using normal vector (9-coefficient reconstruction to final color)
float3 EvaluateSH9(float3 N)
{
    float x = N.x;
    float y = N.y;
    float z = N.z;

    float3 result =
        SHCoefficients[0].xyz +
        SHCoefficients[1].xyz * y +
        SHCoefficients[2].xyz * z +
        SHCoefficients[3].xyz * x +
        SHCoefficients[4].xyz * (x * y) +
        SHCoefficients[5].xyz * (y * z) +
        SHCoefficients[6].xyz * (3.0 * z * z - 1.0) +
        SHCoefficients[7].xyz * (x * z) +
        SHCoefficients[8].xyz * (x * x - y * y);

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

// Roughness-dependent Fresnel term: Attenuating specular intensity for IBL (Roughness-based clamping to offset missing D and G terms)
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
    
    // Adjustable shadow parameters
    float LIGHT_WORLD_SIZE = 1.0;
    float LIGHT_SIZE_UV = LIGHT_WORLD_SIZE / padding3;
    
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
        // Here is a hardware-accelerated 2x2 bilinear interpolation for anti-aliasing
        shadow += tShadowMap.SampleCmpLevelZero(shadowSampler, projCoords.xy + offset, zReceiver).r;
    }
    
    return shadow / 16.0f;
}

// PBR pixel Shader
float4 PSMain(VS_OUTPUT input) : SV_TARGET
{
    uint instMatID = gInstanceData[input.instanceID].customMaterialID;
    uint finalMatID = (instMatID != 0xFFFFFFFF) ? instMatID : materialID;
    
    MaterialData mat = gMaterialData[finalMatID];
    
    Texture2D tAlbedo = ResourceDescriptorHeap[mat.albedoIdx];
    Texture2D tEmissive = ResourceDescriptorHeap[mat.emissiveIdx];
    
    float4 albedoSample = tAlbedo.Sample(s1, input.texCoord);
    float3 albedo = pow(albedoSample.rgb, 2.2);
    float finalAlpha = albedoSample.a;
    
    if (mat.isUnlit)
    {
        float3 unlitColor = hasEmissive ? pow(tEmissive.Sample(s1, input.texCoord).rgb, 2.2) : albedo;
        return float4(unlitColor * finalAlpha, finalAlpha);
    }

#if LOD_LEVEL == 2
    float3 N = normalize(input.normal);
    float3 L = normalize(-lightDir);
    float NdotL = max(dot(N, L), 0.0);
    
    float3 irradiance = EvaluateSH9(N);
    float3 diffuse_IBL = irradiance * albedo;
    float3 directDiffuse = albedo * lightColor * NdotL;
    
    float3 totalDiffuse = diffuse_IBL + directDiffuse;
    return float4(totalDiffuse * finalAlpha, finalAlpha);
#else
    Texture2D tMR = ResourceDescriptorHeap[mat.ormIdx];
    
    float ao = 1.0;
    float roughness = 0.5;
    float metallic = 0.0;
    
    float4 mrSample = tMR.Sample(s1, input.texCoord);
    ao = max(mrSample.r, 0.01);
    roughness = mrSample.g;
    metallic = mrSample.b;
    
    float3 N;
    
#if LOD_LEVEL == 1
    N = normalize(input.normal);
#else
    N = getNormalFromMap(input);
#endif

    float3 V = normalize(camPos - input.worldPos);
    float3 R = reflect(-V, N);
    float3 F0 = float3(0.04, 0.04, 0.04);
    F0 = lerp(F0, albedo, metallic);
    float3 L = normalize(-lightDir);
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
    float3 directDiffuse = (kD * albedo / PI) * radiance * NdotL * shadow;
    float3 directSpecular = specular * radiance * NdotL * shadow;
    
    // Diffuse IBL (SH)
    float3 F_IBL = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    float3 kS_IBL = F_IBL;
    float3 kD_IBL = 1.0 - kS_IBL;
    kD_IBL *= 1.0 - metallic;
    float3 irradiance = EvaluateSH9(N);
    float3 diffuse_IBL = irradiance * albedo;
    float3 specular_IBL = float3(0.0, 0.0, 0.0);
    
#if LOD_LEVEL == 0
    // Specular IBL (split sum)
    TextureCube tPrefilter = ResourceDescriptorHeap[iblPrefilterIdx];
    Texture2D tBRDF = ResourceDescriptorHeap[iblBRDFIdx];
    
    const float MAX_REFLECTION_LOD = 4.0;
    float3 prefilteredColor = tPrefilter.SampleLevel(s1, R, roughness * MAX_REFLECTION_LOD).rgb;
    float2 brdf = tBRDF.Sample(s1, float2(max(dot(N, V), 0.0), roughness)).rg;
    specular_IBL = prefilteredColor * (F0 * brdf.x + brdf.y);
#elif LOD_LEVEL == 1
    TextureCube tPrefilter = ResourceDescriptorHeap[iblPrefilterIdx];

    const float MAX_REFLECTION_LOD = 4.0;
    float3 prefilteredColor = tPrefilter.SampleLevel(s1, R, roughness * MAX_REFLECTION_LOD).rgb;
    specular_IBL = prefilteredColor * F_IBL;
#endif
    
    // The kD_IBL term is decoupled from the split-sum specular BRDF
    // It serves as a visual constraint to mimic energy conservation rather than achieving strict physical correctness
    float3 ambientDiffuse = kD_IBL * diffuse_IBL * ao;
    float3 ambientSpecular = specular_IBL * ao;
    
    // Add emissive (if applicable)
    float3 emissive = hasEmissive ? pow(tEmissive.Sample(s1, input.texCoord).rgb, 2.2) : float3(0.0, 0.0, 0.0);
    
    float3 totalDiffuse = directDiffuse + ambientDiffuse;
    float3 totalSpecular = directSpecular + ambientSpecular;
    
    float3 finalColor = (totalDiffuse * finalAlpha) + totalSpecular + emissive;

    return float4(finalColor, finalAlpha);
#endif
}