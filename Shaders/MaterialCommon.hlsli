#ifndef MATERIAL_COMMON_HLSLI
#define MATERIAL_COMMON_HLSLI

static const float MIN_PERCEPTUAL_ROUGHNESS = 0.005f;

float3 DecodeSRGBColor(float3 color)
{
    return pow(abs(color), 2.2);
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
