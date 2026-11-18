#ifndef SSGI_HIZ_TRACE_HLSLI
#define SSGI_HIZ_TRACE_HLSLI

// Both queries use the same finite-segment traversal and the same global budget.
// The constant query mode is specialized away by DXC at each call site.
// Radiance queries need a front-facing depth hit; endpoint visibility tests both
// faces against the pixel tangent plane so a light's thickness shell cannot block it.
SSGIScreenRayResult TraceSSGIHiZ(uint2 receiver, float3 origin, float3 direction,
    float rayLength, bool endpointVisibility, float endpointTolerance)
{
    SSGIScreenRayResult result = (SSGIScreenRayResult)0;
    result.status = SSGI_SCREEN_INVALID;
    if (any(sceneSize == 0) || any(receiver >= sceneSize) || !all(isfinite(origin))
        || !all(isfinite(direction)) || dot(direction, direction) <= 0.0 || origin.z <= 0.0
        || !isfinite(radius) || radius <= 0.0 || !isfinite(rayLength) || rayLength <= 0.0
        || rayLength > radius || levelCount > 7 || maxIterations == 0
        || !isfinite(endpointTolerance) || endpointTolerance < 0.0) return result;

    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    Texture2D<float4> normals = ResourceDescriptorHeap[normalTextureIdx];
    float3 start = Project(origin);
    float3 delta = Project(origin + direction * rayLength) - start;
    if (!all(isfinite(start)) || !all(isfinite(delta))) return result;
    float endT = 1.0;
    [unroll] for (uint axis = 0; axis < 2; ++axis)
    {
        if (delta[axis] > 0) endT = min(endT, (1 - start[axis]) / delta[axis]);
        if (delta[axis] < 0) endT = min(endT, -start[axis] / delta[axis]);
    }
    if (any(start.xy < 0) || any(start.xy >= 1) || endT <= 0
        || (endpointVisibility && endT < 1.0 - 1e-5))
    {
        result.status = SSGI_SCREEN_OUT_OF_SCREEN;
        return result;
    }
    // An endpoint within the viewport rounding tolerance is still a full segment.
    if (endpointVisibility) endT = 1.0;

    float2 startPixels = start.xy * sceneSize;
    float2 deltaPixels = delta.xy * sceneSize;
    float2 inverseDelta = float2(abs(deltaPixels.x) > 1e-8 ? rcp(deltaPixels.x) : 0,
        abs(deltaPixels.y) > 1e-8 ? rcp(deltaPixels.y) : 0);
    int2 firstCell = RayCell(startPixels, deltaPixels, inverseDelta, 0.0, 0);
    // Skip only the receiver's own fine cell. A biased origin may project into
    // a neighboring cell, which still has to be checked for a real blocker.
    float t = all(firstCell == int2(receiver))
        ? CellExit(startPixels, deltaPixels, inverseDelta, firstCell, 0, endT) : 0.0;
    uint mip = 0;
    uint iteration = 0;
    [loop] for (; iteration < maxIterations && t < endT; ++iteration)
    {
        int2 cell = RayCell(startPixels, deltaPixels, inverseDelta, t, mip);
        uint cellSize = 1u << mip;
        int2 gridSize = int2((sceneSize + cellSize - 1) / cellSize);
        if (any(cell < 0) || any(cell >= gridSize))
        {
            result.status = SSGI_SCREEN_OUT_OF_SCREEN;
            return result;
        }
        float exitT = CellExit(startPixels, deltaPixels, inverseDelta, cell, mip, endT);
        float2 bounds;
        if (mip == 0)
        {
            float z = depth.Load(int3(cell, 0));
            float viewZ = ReconstructPosition((cell + 0.5) / sceneSize, z, invProjMat).z;
            bounds = z < 1.0 ? viewZ.xx : float2(1e20, 0);
        }
        else
        {
            Texture2D<float2> hierarchy = ResourceDescriptorHeap[NonUniformResourceIndex(levels[mip - 1].x)];
            // Physical cell size preserves alignment at odd image edges.
            bounds = hierarchy.Load(int3(cell, 0));
        }

        float entryT = t;
        bool potentialHit = false;
        if (bounds.x <= bounds.y)
        {
            float2 slab = float2(bounds.x, bounds.y + thickness);
            if (endpointVisibility)
            {
                // Coarse visibility bounds conservatively include one pixel's
                // depth span; fine cells use their finite tangent plane below.
                float padding = max(0.001, (bounds.y - bounds.x) / max(float(cellSize) - 1.0, 1.0));
                slab = float2(max(bounds.x - padding, 0.001), bounds.y + padding);
            }
            potentialHit = DepthInterval(start.z, delta.z, slab, t, exitT, entryT);
        }
        if (potentialHit && mip > 0)
        {
            t = entryT;
            --mip;
            continue;
        }
        if (mip == 0 && bounds.x <= bounds.y && any(uint2(cell) != receiver))
        {
            if (endpointVisibility)
            {
                float3 worldNormal = SSGIReceiverGeometricNormal(uint2(cell),
                    normalize(DecodeGBufferNormal(normals.Load(int3(cell, 0)).xyz)));
                float3 normal = normalize(mul(float4(worldNormal, 0), viewMat).xyz);
                float3 planePoint = ReconstructPosition((cell + 0.5) / sceneSize,
                    depth.Load(int3(cell, 0)), invProjMat);
                if (!all(isfinite(normal)) || !all(isfinite(planePoint))) return result;
                float denominator = dot(normal, direction);
                if (abs(denominator) > 1e-6)
                {
                    float hitDistance = dot(planePoint - origin, normal) / denominator;
                    if (hitDistance > 0 && hitDistance < rayLength - endpointTolerance)
                    {
                        float3 hit = origin + direction * hitDistance;
                        float2 pixel = Project(hit).xy * sceneSize;
                        if (all(pixel >= float2(cell) - 0.001) && all(pixel <= float2(cell + 1) + 0.001))
                        {
                            result.hitPixel = uint2(cell);
                            result.viewPoint = planePoint;
                            result.rayPoint = hit;
                            result.distance = hitDistance;
                            result.status = SSGI_SCREEN_HIT;
                            result.confidence = 1.0;
                            return result;
                        }
                    }
                }
            }
            else if (potentialHit)
            {
                float2 hitUV = start.xy + delta.xy * entryT;
                float3 hit = ReconstructPosition(hitUV, DeviceDepth(bounds.x), invProjMat);
                float3 hitNormal = SSGIDecodeViewNormal(normals.Load(int3(cell, 0)).xyz, viewMat);
                if (distance(hit, origin) <= radius && dot(hitNormal, direction) < 0)
                {
                    result.hitPixel = uint2(cell);
                    result.viewPoint = ReconstructPosition((cell + 0.5) / sceneSize,
                        depth.Load(int3(cell, 0)), invProjMat);
                    result.rayPoint = hit;
                    result.distance = distance(hit, origin);
                    result.status = SSGI_SCREEN_HIT;
                    result.confidence = 1.0;
                    return result;
                }
            }
        }
        t = exitT;
        mip = min(mip + 1, levelCount);
    }
    result.status = t < endT ? SSGI_SCREEN_ITERATION_LIMIT
        : endT < 1.0 ? SSGI_SCREEN_OUT_OF_SCREEN : SSGI_SCREEN_RADIUS_LIMIT;
    return result;
}

SSGIScreenRayResult TraceScreenRay(uint2 receiver, float3 origin, float3 direction)
{
    return TraceSSGIHiZ(receiver, origin, direction, SSGIViewRayLength(origin, direction), false, 0.0);
}

bool ScreenEmissiveVisible(uint2 receiver, float3 origin, float3 direction, float distance, float endpointTolerance)
{
    SSGIScreenRayResult result = TraceSSGIHiZ(receiver, origin, direction, distance, true, endpointTolerance);
    // Reaching the endpoint is the only visibility proof. A truncated or invalid
    // query contributes no emitter sample, even when no blocker was found yet.
    return result.status == SSGI_SCREEN_RADIUS_LIMIT;
}

#endif
