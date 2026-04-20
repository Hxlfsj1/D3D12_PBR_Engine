cbuffer PassConstants : register(b0)
{
    float3 camPos;
    float padding1;
    float3 lightPos;
    float padding2;
    float3 lightColor;
    float padding3;
};

Texture2D tAlbedo : register(t0);
Texture2D tNormal : register(t1);
Texture2D tMR : register(t2);
Texture2D tEmissive : register(t3);
TextureCube tPrefilter : register(t4);
Texture2D tBRDF : register(t5);

struct InstanceData
{
    float4x4 wvpMat;
    float4x4 worldMat;
    float4x4 normalMat;
};
StructuredBuffer<InstanceData> gInstanceData : register(t6);

SamplerState s1 : register(s0);

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
    float distance = length(lightPos - input.worldPos);
    float attenuation = 1.0 / (distance * distance + 0.001);
    float3 radiance = lightColor * attenuation;

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

    // Calculate final light
    float NdotL = max(dot(N, L), 0.0);
    float3 Lo = (kD * albedo / PI + specular) * radiance * NdotL;
    
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