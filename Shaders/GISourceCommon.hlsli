#ifndef GI_SOURCE_COMMON_HLSLI
#define GI_SOURCE_COMMON_HLSLI

#include "MathCommon.hlsli"
#include "MaterialCommon.hlsli"

// One camera-independent outgoing diffuse source contract for screen and world
// hits. Receiving-material response is applied later, once, by the GI receiver.
// No environment SH, AO, specular, view-vector Fresnel or GI feedback enters Lo0.
float3 GIComputeSourceDiffuseWeight(float3 albedo, float metallic)
{
    return ComputeDiffuseEnergy(ComputeMaterialF0(albedo, metallic), metallic);
}

float3 GICapSourceEmissive(float3 emissive)
{
    const float maximumSourceEmission = 2.0;
    float peak = max(emissive.r, max(emissive.g, emissive.b));
    return emissive * (maximumSourceEmission / max(maximumSourceEmission, peak));
}

float3 GIComputeSourceDirectDiffuse(float3 albedo, float metallic,
    float3 sunRadiance, float NdotL, float visibility)
{
    return GIComputeSourceDiffuseWeight(albedo, metallic) * albedo / PI
        * sunRadiance * max(NdotL, 0.0) * visibility;
}

float3 GIComputeSourceRadiance(float3 albedo, float metallic, float3 sunRadiance,
    float NdotL, float visibility, float3 emissive, bool isUnlit)
{
    // Preserve the existing screen contract: unlit surfaces terminate rays but
    // contribute no diffuse/emissive GI source. Their visible albedo is separate.
    if (isUnlit) return 0.0;
    return GIComputeSourceDirectDiffuse(albedo, metallic, sunRadiance, NdotL, visibility)
        + GICapSourceEmissive(emissive);
}

#endif
