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

#endif
