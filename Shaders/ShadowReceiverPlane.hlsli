#ifndef SHADOW_RECEIVER_PLANE_HLSLI
#define SHADOW_RECEIVER_PLANE_HLSLI

float2 CalculateShadowReceiverDepthGradient(float3 positionDDX, float3 positionDDY)
{
    if (!all(isfinite(positionDDX)) || !all(isfinite(positionDDY)))
        return 0;

    // Derivative magnitudes depend on resolution and projection. Remove their
    // scales before crossing; an absolute epsilon on the raw cross product
    // suppresses valid receiver-plane compensation at small pixel footprints.
    float scaleX = max(max(abs(positionDDX.x), abs(positionDDX.y)), abs(positionDDX.z));
    float scaleY = max(max(abs(positionDDY.x), abs(positionDDY.y)), abs(positionDDY.z));
    if (scaleX == 0 || scaleY == 0)
        return 0;

    float3 planeNormal = cross(positionDDX / scaleX, positionDDY / scaleY);
    float normalScale = max(max(abs(planeNormal.x), abs(planeNormal.y)), abs(planeNormal.z));
    if (normalScale == 0)
        return 0;
    planeNormal /= normalScale;

    // Keep the existing relative grazing-angle limit. A nonzero, rescaled
    // normal makes this denominator positive without a footprint-sized floor.
    float denominator = max(abs(planeNormal.z), length(planeNormal) * 0.0872665f);
    denominator = planeNormal.z < 0 ? -denominator : denominator;
    return -planeNormal.xy / denominator;
}

#endif
