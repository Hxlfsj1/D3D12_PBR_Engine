#ifndef MATERIAL_COMMON_HLSLI
#define MATERIAL_COMMON_HLSLI

static const float MIN_PERCEPTUAL_ROUGHNESS = 0.005f;
static const float3 DIELECTRIC_F0 = float3(0.04f, 0.04f, 0.04f);
// A missing normal map uses the geometric shading normal, without a dummy
// texture or a tangent basis.
static const uint NO_NORMAL_TEXTURE_INDEX = 0xFFFFFFFFu;

float3 ApplyMaterialORMFactors(float3 orm, float metallicFactor, float roughnessFactor)
{
    return float3(orm.r, saturate(orm.g * roughnessFactor), saturate(orm.b * metallicFactor));
}

// glTF occlusion and metallic/roughness may use separate images. Even when
// they share one packed image, AO comes from its own binding and R channel.
float3 ComposeMaterialORM(float3 mrSample, float occlusionSample,
    float metallicFactor, float roughnessFactor, float occlusionStrength)
{
    float3 orm = ApplyMaterialORMFactors(mrSample, metallicFactor, roughnessFactor);
    orm.r = lerp(1.0f, saturate(occlusionSample), saturate(occlusionStrength));
    return orm;
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

float ClampPerceptualRoughness(float roughness)
{
    return max(roughness, MIN_PERCEPTUAL_ROUGHNESS);
}

float3 FaceNormalByFrontFace(float3 normal, bool isFrontFace)
{
    return isFrontFace ? normal : -normal;
}

float3 EncodeGBufferNormal(float3 normal)
{
    return normal * 0.5f + 0.5f;
}

float3 DecodeGBufferNormal(float3 encodedNormal)
{
    return encodedNormal * 2.0f - 1.0f;
}

float4 BuildSurfaceOutput(float3 color, float alpha)
{
#ifdef TRANSPARENT_PASS
    return float4(color * alpha, alpha);
#else
    return float4(color, 1.0f);
#endif
}

#endif
