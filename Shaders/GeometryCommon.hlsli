#ifndef GEOMETRY_COMMON_HLSLI
#define GEOMETRY_COMMON_HLSLI

// Top-left UV origin, ordinary D3D device depth. Matrices use mul(vector, matrix).
// Inverse projection gives view space; inverse view-projection gives world space.
// Callers requiring a validity check inspect w before the perspective divide.
float4 ReconstructPositionH(float2 uv, float depth, float4x4 inverseClipTransform)
{
    float4 clipPosition = float4(uv * float2(2, -2) + float2(-1, 1), depth, 1);
    return mul(clipPosition, inverseClipTransform);
}

float3 ReconstructPosition(float2 uv, float depth, float4x4 inverseClipTransform)
{
    float4 positionH = ReconstructPositionH(uv, depth, inverseClipTransform);
    return positionH.xyz / positionH.w;
}

// One-pixel spacing at constant device depth, not distance along a sloped surface.
// Position must use the same coordinate space as inverseClipTransform.
float SameDepthPixelFootprint(float2 uv, float depth, float3 position, uint2 sceneSize,
                             float4x4 inverseClipTransform)
{
    float footprintX = length(ReconstructPosition(uv + float2(1.0 / sceneSize.x, 0), depth,
                                                    inverseClipTransform) - position);
    float footprintY = length(ReconstructPosition(uv + float2(0, 1.0 / sceneSize.y), depth,
                                                    inverseClipTransform) - position);
    return max(footprintX, footprintY);
}

// Both normals must have unit length and share the displacement's coordinate space.
float SymmetricPlaneDistance(float3 delta, float3 normal, float3 sampleNormal)
{
    return max(abs(dot(delta, normal)), abs(dot(delta, sampleNormal)));
}

#endif
