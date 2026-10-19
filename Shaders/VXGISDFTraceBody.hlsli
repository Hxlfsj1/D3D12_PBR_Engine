// Shared DDA traversal for geometry-only and material-aware overloads.
// Packed dense cells retain their original CSR IDs for invalid-coverage fallback.

    hit = (VXGISDFHit)0;
    hit.triangleId = 0xffffffff;
    hit.triangleIndex = 0xffffffff;
    float maximumDirection = max(abs(rayDirection.x), max(abs(rayDirection.y), abs(rayDirection.z)));
    if (!all(isfinite(rayOrigin)) || !all(isfinite(rayDirection)) || maximumDirection == 0.0
        || !isfinite(rayMin) || !isfinite(rayMax) || rayMin < 0.0 || rayMax < rayMin)
    {
        hit.traceStatus = 2;
        return false;
    }
    rayDirection = normalize(rayDirection / maximumDirection);
    float entry, exit;
    if (!VXGISDFRayBounds(rayOrigin, rayDirection, grid, rayMin, rayMax, entry, exit)) return false;
    float epsilon = max(grid.cellSize * 1e-5, 1e-6);
    float current = entry;
    int3 cell, step;
    float3 boundaryDistance, deltaDistance;
    VXGISDFInitializeDDA(rayOrigin, rayDirection, current, grid, epsilon, cell, step, boundaryDistance, deltaDistance);
    uint iterationLimit = max(1024u, 4u * (grid.cellDimensions.x +
        grid.cellDimensions.y + grid.cellDimensions.z) + 16u);
    [loop] for (uint iteration = 0; iteration < iterationLimit; ++iteration)
    {
        if (any(cell < 0) || any(cell >= int3(grid.cellDimensions)) || current > exit + epsilon) return false;
        float cellExit = min(exit, min(boundaryDistance.x, min(boundaryDistance.y, boundaryDistance.z)));
        uint cellIndex = VXGISDFFlatIndex(uint3(cell), grid.cellDimensions);
        uint cellBegin = cellOffsets[cellIndex];
        uint cellEnd = cellOffsets[cellIndex + 1];
        bool useBVH = cellBegin < cellEnd && triangleReferences[cellBegin] == 0x80000001u;
        uint linearBegin = cellBegin;
        uint linearEnd = cellEnd;
        uint node = cellEnd;
        if (useBVH)
        {
            linearBegin = cellBegin + 3;
            linearEnd = linearBegin + triangleReferences[cellBegin + 1];
            node = triangleReferences[cellBegin + 2];
        }
        uint linearCursor = linearBegin;
        uint leafCursor = 0, leafEnd = 0;
        float closest = cellExit + epsilon;
        uint closestIndex = 0;
        float3 closestBarycentric = 0.0;
        bool found = false;
        float intervalMin = max(rayMin, current - epsilon);
        float intervalMax = min(rayMax, cellExit + epsilon);
        [loop] while (true)
        {
            uint triangleIndex;
            if (useBVH)
            {
                [loop] while (leafCursor == leafEnd && node < cellEnd)
                {
                    float3 minimum = asfloat(uint3(triangleReferences[node], triangleReferences[node + 1], triangleReferences[node + 2]));
                    float3 maximum = asfloat(uint3(triangleReferences[node + 3], triangleReferences[node + 4], triangleReferences[node + 5]));
                    uint escape = triangleReferences[node + 6];
                    uint count = triangleReferences[node + 7];
                    // Test the whole cell interval, not the current nearest hit.
                    // Every potentially malformed alpha must reach the fallback.
                    if (!VXGISDFRayCandidateBounds(rayOrigin, rayDirection, minimum, maximum, intervalMin, intervalMax))
                    {
                        node = escape;
                        continue;
                    }
                    if (count == 0) node += 8;
                    else
                    {
                        leafCursor = node + 8;
                        leafEnd = leafCursor + count;
                        node = escape;
                    }
                }
                if (leafCursor == leafEnd) break;
                triangleIndex = triangleReferences[leafCursor++];
            }
            else
            {
                if (linearCursor >= linearEnd) break;
                triangleIndex = triangleReferences[linearCursor++];
            }
            float triangleDistance;
            float3 barycentric;
            if (!(VXGISDFRayTriangle(rayOrigin, rayDirection, triangles[triangleIndex], triangleDistance, barycentric)
                && triangleDistance >= rayMin && triangleDistance <= rayMax
                && triangleDistance >= current - epsilon && triangleDistance <= cellExit + epsilon)) continue;
            bool nearer = !found || triangleDistance < closest || (triangleDistance == closest && triangleIndex < closestIndex);
#ifdef VXGI_SDF_TRACE_COVERAGE
            if (useBVH || nearer)
            {
                uint coverage = VXGISDFCandidateCoverage(triangles[triangleIndex], barycentric,
                    materials, materialCount, materialSampler, materialMipLevel);
                if (coverage == 2)
                {
                    if (useBVH)
                    {
                        // Replay the original CSR order on malformed coverage.
                        // This preserves the original fail-closed semantics even
                        // when the bad candidate lies behind a valid nearer hit.
                        useBVH = false;
                        linearCursor = linearBegin;
                        found = false;
                        closest = cellExit + epsilon;
                        closestIndex = 0;
                        closestBarycentric = 0.0;
                        continue;
                    }
                    hit.traceStatus = 2;
                    return false;
                }
                if (coverage == 0) continue;
            }
#endif
            if (!nearer) continue;
            closest = triangleDistance;
            closestIndex = triangleIndex;
            closestBarycentric = barycentric;
            found = true;
        }
        if (found)
        {
            VXGISDFTriangle surfaceTriangle = triangles[closestIndex];
            hit.position = rayOrigin + rayDirection * closest;
            hit.distance = closest;
            hit.geometricNormal = surfaceTriangle.geometricNormal;
            hit.triangleId = surfaceTriangle.triangleId;
            hit.triangleIndex = closestIndex;
            hit.barycentric = closestBarycentric;
            hit.backFace = dot(rayDirection, surfaceTriangle.geometricNormal) > 0.0 ? 1 : 0;
            hit.valid = 1;
            hit.traceStatus = 1;
            return true;
        }
        if (cellExit >= exit) return false;
        float safeStep = 0.9 * VXGISDFConservativeDistance(rayOrigin + rayDirection * current, grid, nodes);
        if (current + safeStep > cellExit + epsilon)
        {
            current += safeStep;
            if (current > exit) return false;
            VXGISDFInitializeDDA(rayOrigin, rayDirection, current, grid, epsilon, cell, step, boundaryDistance, deltaDistance);
        }
        else
        {
            current = cellExit;
            [unroll] for (uint axis = 0; axis < 3; ++axis)
                if (boundaryDistance[axis] <= cellExit)
                {
                    cell[axis] += step[axis];
                    boundaryDistance[axis] += deltaDistance[axis];
                }
        }
    }
    hit.traceStatus = 3;
    return false;
