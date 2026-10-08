#ifndef SSGI_COMMON_HLSLI
#define SSGI_COMMON_HLSLI

#include "MaterialCommon.hlsli"
#include "GeometryCommon.hlsli"

float3 SSGIDecodeViewNormal(float3 encodedNormal, float4x4 viewMat)
{
    return normalize(mul(float4(DecodeGBufferNormal(encodedNormal), 0), viewMat).xyz);
}

// One-pixel spacing at constant device depth, not distance along a sloped surface.
// Position must use the same coordinate space as inverseClipTransform.
float SSGISameDepthPixelFootprint(float2 uv, float depth, float3 position, uint2 sceneSize,
                                float4x4 inverseClipTransform)
{
    float footprintX = length(ReconstructPosition(uv + float2(1.0 / sceneSize.x, 0), depth,
                                                    inverseClipTransform) - position);
    float footprintY = length(ReconstructPosition(uv + float2(0, 1.0 / sceneSize.y), depth,
                                                    inverseClipTransform) - position);
    return max(footprintX, footprintY);
}

// Both normals and the displacement must be expressed in the same coordinate space.
float SSGISymmetricPlaneDistance(float3 delta, float3 normal, float3 sampleNormal)
{
    return max(abs(dot(delta, normal)), abs(dot(delta, sampleNormal)));
}

// Reconstruct/Composite share these geometry weights, but choose their own spatial
// kernel and rejection thresholds. Keep the multiplication order of the original filters.
float SSGIApplyBilateralGeometryWeight(float spatialWeight, float normalAgreement, float planeError)
{
    return spatialWeight * pow(normalAgreement, 32) * exp2(-4 * planeError * planeError);
}

#endif
