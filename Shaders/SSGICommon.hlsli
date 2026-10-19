#ifndef SSGI_COMMON_HLSLI
#define SSGI_COMMON_HLSLI

#include "MaterialCommon.hlsli"
#include "GeometryCommon.hlsli"

float3 SSGIDecodeViewNormal(float3 encodedNormal, float4x4 viewMat)
{
    return normalize(mul(float4(DecodeGBufferNormal(encodedNormal), 0), viewMat).xyz);
}

// Reconstruct/Composite share these geometry weights, but choose their own spatial
// kernel and rejection thresholds. Keep the multiplication order of the original filters.
float SSGIApplyBilateralGeometryWeight(float spatialWeight, float normalAgreement, float planeError)
{
    return spatialWeight * pow(normalAgreement, 32) * exp2(-4 * planeError * planeError);
}

#endif
