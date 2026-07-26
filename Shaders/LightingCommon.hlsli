#ifndef LIGHTING_COMMON_HLSLI
#define LIGHTING_COMMON_HLSLI

#include "MathCommon.hlsli"
#include "MaterialCommon.hlsli"

static const float3 DIELECTRIC_F0 = float3(0.04f, 0.04f, 0.04f);
static const float SPECULAR_IBL_MAX_MIP = 4.0f;

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
    return GeometrySchlickGGX(max(dot(N, V), 0.0), roughness) *
        GeometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

float3 fresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float3 fresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    return F0 + (max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), F0) - F0) *
        pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float3 ComputeMaterialF0(float3 albedo, float metallic)
{
    return lerp(DIELECTRIC_F0, albedo, metallic);
}

float3 ComputeDiffuseEnergy(float3 F, float metallic)
{
    return (1.0f - F) * (1.0f - metallic);
}

float3 ComputeCookTorranceSpecular(float3 N, float3 V, float3 L, float3 F, float roughness)
{
    float3 H = normalize(V + L);
    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    float3 numerator = NDF * G * F;
    float denominator = 4.0f * max(dot(N, V), 0.0f) * max(dot(N, L), 0.0f) + 0.0001f;
    return numerator / denominator;
}

float3 ComputeDirectDiffuse(float3 kD, float3 albedo, float3 radiance, float NdotL, float shadow)
{
    return (kD * albedo / PI) * radiance * NdotL * shadow;
}

float3 ComputeDirectSpecular(float3 specular, float3 radiance, float NdotL, float shadow)
{
    return specular * radiance * NdotL * shadow;
}

float3 ComputeSplitSumSpecularIBL(float3 prefilteredColor, float2 brdf, float3 F_IBL)
{
    return prefilteredColor * (F_IBL * brdf.x + brdf.y);
}

float3 ComputeSimpleSpecularIBL(float3 prefilteredColor, float3 F_IBL)
{
    return prefilteredColor * F_IBL;
}

uint SelectCascadeIndex(float dist)
{
    uint cascadeIndex = 0;
    if (dist > cascadeSplits.x)
        cascadeIndex = 1;
    if (dist > cascadeSplits.y)
        cascadeIndex = 2;
    if (dist > cascadeSplits.z)
        cascadeIndex = 3;

    return cascadeIndex;
}

float FadeCascadeShadow(float shadow, float dist)
{
    float fadeDistance = 10.0f;
    float fadeStart = cascadeSplits.w - fadeDistance;
    float fadeFactor = saturate((dist - fadeStart) / fadeDistance);
    return lerp(shadow, 1.0f, fadeFactor);
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

float CalculateReceiverPlaneCompareDepth(
    float zReceiver,
    float2 sampleOffset,
    float2 receiverDepthGradient)
{
    float sampleReceiverDepth = zReceiver + dot(receiverDepthGradient, sampleOffset);
    return min(zReceiver, sampleReceiverDepth);
}

void FindBlocker(
    out float avgBlockerDepth,
    out float numBlockers,
    float2 uv,
    float zReceiver,
    float2 receiverDepthGradient,
    float searchRadius,
    uint cascadeIndex)
{
    Texture2DArray tShadowMap = ResourceDescriptorHeap[shadowMapIdx];
    float blockerSum = 0.0;
    numBlockers = 0.0;

    float randomAngle = Rand(uv) * 2.0 * PI;
    float cosTheta = cos(randomAngle);
    float sinTheta = sin(randomAngle);
    float2x2 rotMat = float2x2(cosTheta, -sinTheta, sinTheta, cosTheta);

    for (int i = 0; i < 16; ++i)
    {
        float2 offset = mul(POISSON_DISK[i], rotMat) * searchRadius;
        float shadowMapDepth = tShadowMap.SampleLevel(
            shadowDepthPointSampler,
            float3(uv + offset, (float) cascadeIndex),
            0).r;
        float sampleCompareDepth = CalculateReceiverPlaneCompareDepth(
            zReceiver,
            offset,
            receiverDepthGradient);

        if (shadowMapDepth < sampleCompareDepth)
        {
            blockerSum += shadowMapDepth;
            numBlockers += 1.0;
        }
    }

    avgBlockerDepth = numBlockers > 0.0 ? blockerSum / numBlockers : 1.0;
}

float CalcShadowFactor(
    float4 lightSpacePos,
    float4 lightSpacePosDDX,
    float4 lightSpacePosDDY,
    uint cascadeIndex)
{
    Texture2DArray tShadowMap = ResourceDescriptorHeap[shadowMapIdx];

    float inverseW = rcp(lightSpacePos.w);
    float3 projCoords = lightSpacePos.xyz * inverseW;
    float3 projCoordsDDX = (lightSpacePosDDX.xyz - projCoords * lightSpacePosDDX.w) * inverseW;
    float3 projCoordsDDY = (lightSpacePosDDY.xyz - projCoords * lightSpacePosDDY.w) * inverseW;

    projCoords.x = projCoords.x * 0.5f + 0.5f;
    projCoords.y = -projCoords.y * 0.5f + 0.5f;
    projCoordsDDX.xy *= float2(0.5f, -0.5f);
    projCoordsDDY.xy *= float2(0.5f, -0.5f);

    if (projCoords.z > 1.0f || projCoords.x < 0.0f || projCoords.x > 1.0f || projCoords.y < 0.0f || projCoords.y > 1.0f)
        return 1.0f;

    float zReceiver = projCoords.z;

    float3 receiverPlaneNormal = cross(projCoordsDDX, projCoordsDDY);
    float receiverPlaneNormalLength = length(receiverPlaneNormal);
    float receiverPlaneDenominator = max(
        max(abs(receiverPlaneNormal.z), receiverPlaneNormalLength * 0.0872665f),
        1e-6f);
    receiverPlaneDenominator = receiverPlaneNormal.z < 0.0f
        ? -receiverPlaneDenominator
        : receiverPlaneDenominator;
    float2 receiverDepthGradient = -receiverPlaneNormal.xy / receiverPlaneDenominator;

    float currentOrthoWidth = 1.0;
    float currentDepthRange = 1.0;
    if (cascadeIndex == 0)
    {
        currentOrthoWidth = cascadeOrthoWidths.x;
        currentDepthRange = cascadeDepthRanges.x;
    }
    else if (cascadeIndex == 1)
    {
        currentOrthoWidth = cascadeOrthoWidths.y;
        currentDepthRange = cascadeDepthRanges.y;
    }
    else if (cascadeIndex == 2)
    {
        currentOrthoWidth = cascadeOrthoWidths.z;
        currentDepthRange = cascadeDepthRanges.z;
    }
    else if (cascadeIndex == 3)
    {
        currentOrthoWidth = cascadeOrthoWidths.w;
        currentDepthRange = cascadeDepthRanges.w;
    }

    float inverseOrthoWidth = rcp(max(currentOrthoWidth, 0.001f));
    float receiverSearchDistance = max(zReceiver, 0.0f) * currentDepthRange;

    float avgBlockerDepth = 1.0;
    float numBlockers = 0.0;
    float searchRadius = receiverSearchDistance * tanSunAngularRadius * inverseOrthoWidth;
    FindBlocker(
        avgBlockerDepth,
        numBlockers,
        projCoords.xy,
        zReceiver,
        receiverDepthGradient,
        searchRadius,
        cascadeIndex);

    if (numBlockers < 1.0)
        return 1.0f;

    float blockerReceiverDistance = max(zReceiver - avgBlockerDepth, 0.0f) * currentDepthRange;
    float filterRadius = blockerReceiverDistance * tanSunAngularRadius * inverseOrthoWidth;

    float shadow = 0.0f;
    float randomAngle = Rand(projCoords.xy + float2(1.0, 1.0)) * 2.0 * PI;
    float cosTheta = cos(randomAngle);
    float sinTheta = sin(randomAngle);
    float2x2 rotMat = float2x2(cosTheta, -sinTheta, sinTheta, cosTheta);

    for (int i = 0; i < 16; ++i)
    {
        float2 offset = mul(POISSON_DISK[i], rotMat) * filterRadius;
        float sampleCompareDepth = CalculateReceiverPlaneCompareDepth(
            zReceiver,
            offset,
            receiverDepthGradient);
        shadow += tShadowMap.SampleCmpLevelZero(
            shadowSampler,
            float3(projCoords.xy + offset, (float) cascadeIndex),
            sampleCompareDepth).r;
    }

    return shadow / 16.0f;
}

#endif
