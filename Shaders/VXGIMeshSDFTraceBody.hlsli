// Included inside both VXGISDFTrace overloads. Acceleration selects candidates
// in local space; only an exact world-space test may accept a finite ray hit.
    hit = (VXGISDFHit)0;
    hit.triangleId = 0xffffffffu;
    hit.triangleIndex = 0xffffffffu;
    hit.instanceIndex = 0xffffffffu;
    float directionMaximum = max(abs(rayDirection.x), max(abs(rayDirection.y), abs(rayDirection.z)));
    if (!all(isfinite(rayOrigin)) || !all(isfinite(rayDirection)) || directionMaximum == 0.0
        || !isfinite(rayMin) || !isfinite(rayMax) || rayMin < 0.0 || rayMax < rayMin)
    {
        hit.traceStatus = 2;
        return false;
    }
    rayDirection = normalize(rayDirection / directionMaximum);
    uint instanceCount = VXGIMeshSDFInstances[0].sceneInstanceCount;
    double closestWorld = double(rayMax);
    bool found = false;
    [loop] for (uint instanceIndex = 0; instanceIndex < instanceCount; ++instanceIndex)
    {
        VXGIMeshSDFInstance instance = VXGIMeshSDFInstances[instanceIndex];
        // A necessary overlap test needs no slab divisions. Most short SSGI
        // segments leave the geometry envelope immediately (notably floors).
        // Roundoff expansion only admits extra candidates; exact bounds and
        // triangle acceptance below keep the original world interval.
        float segmentLimit = float(closestWorld);
        float3 segmentStart = rayOrigin + rayDirection * rayMin;
        float3 segmentEnd = rayOrigin + rayDirection * segmentLimit;
        float3 segmentError = 8.0 * 1.1920928955078125e-7 *
            (abs(rayOrigin) + abs(rayDirection) * max(abs(rayMin), abs(segmentLimit))) + 1e-6;
        if (any(min(segmentStart, segmentEnd) - segmentError > instance.boundsMaximum)
            || any(max(segmentStart, segmentEnd) + segmentError < instance.boundsMinimum)) continue;
        if (!VXGIMeshSDFWorldBounds(rayOrigin, rayDirection, instance.boundsMinimum, instance.boundsMaximum,
            double(rayMin), closestWorld)) continue;
        VXGIMeshSDFAsset asset = VXGIMeshSDFAssets[instance.assetIndex];
        if (asset.triangleCount == 0 || any(asset.cellDimensions == 0)) continue;
        VXGISDFGrid localGrid = VXGIMeshSDFGrid(asset);
        float3 localOrigin = mul(float4(rayOrigin, 1), instance.localFromWorld).xyz;
        float3 transformedDirection = mul(float4(rayDirection, 0), instance.localFromWorld).xyz;
        float distanceScale = length(transformedDirection);
        if (!all(isfinite(localOrigin)) || !isfinite(distanceScale) || distanceScale <= 0.0)
        {
            hit.valid = 0;
            hit.traceStatus = 2;
            return false;
        }
        float3 localDirection = transformedDirection / distanceScale;
        // This padding affects candidate selection only. Every candidate must
        // still pass [rayMin, closestWorld] using its world-space intersection.
        float localMagnitude = max(max(abs(localOrigin.x), abs(localOrigin.y)), abs(localOrigin.z));
        float extentMagnitude = max(max(abs(asset.origin.x), abs(asset.origin.y)), abs(asset.origin.z));
        extentMagnitude = max(extentMagnitude, max(max(abs(asset.boundsMaximum.x), abs(asset.boundsMaximum.y)), abs(asset.boundsMaximum.z)));
        float3 originArithmetic = mul(abs(float4(rayOrigin, 1)), abs(instance.localFromWorld)).xyz;
        localMagnitude = max(localMagnitude, max(originArithmetic.x, max(originArithmetic.y, originArithmetic.z)));
        float epsilon = max(asset.cellSize * 1e-5, max(1e-6,
            (localMagnitude + extentMagnitude) * 4e-6));
        float candidateMinimum = float(double(rayMin) * double(distanceScale)) - epsilon;
        float candidateMaximum = float(closestWorld * double(distanceScale)) + epsilon;
        float entry, exit;
        if (!VXGISDFRayBounds(localOrigin, localDirection, localGrid,
            candidateMinimum, candidateMaximum, entry, exit)) continue;
        float current = entry;
        int3 cell, step;
        float3 boundaryDistance, deltaDistance;
        VXGISDFInitializeDDA(localOrigin, localDirection, current, localGrid, epsilon,
            cell, step, boundaryDistance, deltaDistance);
        bool completed = false;
        // Auto-sized assets may have a long axis beyond the old 256-node cap.
        // Match the CPU DDA's finite grid-derived allowance, retaining the old
        // minimum for existing fixtures and their roundoff behavior.
        uint iterationLimit = max(1024u, 4u * (asset.cellDimensions.x +
            asset.cellDimensions.y + asset.cellDimensions.z) + 16u);
        [loop] for (uint iteration = 0; iteration < iterationLimit; ++iteration)
        {
            if (any(cell < 0) || any(cell >= int3(asset.cellDimensions)) || current > exit + epsilon
                || current > float(closestWorld * double(distanceScale)) + epsilon)
            {
                completed = true;
                break;
            }
            float cellExit = min(exit, min(boundaryDistance.x, min(boundaryDistance.y, boundaryDistance.z)));
            uint cellIndex = VXGISDFFlatIndex(uint3(cell), asset.cellDimensions);
            uint cellBegin = cellOffsets[asset.cellOffset + cellIndex];
            uint cellEnd = cellOffsets[asset.cellOffset + cellIndex + 1];
            bool useBVH = cellBegin < cellEnd && triangleReferences[asset.referenceOffset + cellBegin] == 0x80000001u;
            uint linearBegin = cellBegin, linearEnd = cellEnd, node = cellEnd;
            if (useBVH)
            {
                linearBegin = cellBegin + 3;
                linearEnd = linearBegin + triangleReferences[asset.referenceOffset + cellBegin + 1];
                node = triangleReferences[asset.referenceOffset + cellBegin + 2];
            }
            uint linearCursor = linearBegin, leafCursor = 0, leafEnd = 0;
            [loop] while (true)
            {
                uint localTriangleIndex;
                if (useBVH)
                {
                    [loop] while (leafCursor == leafEnd && node < cellEnd)
                    {
                        uint address = asset.referenceOffset + node;
                        float3 minimum = asfloat(uint3(triangleReferences[address], triangleReferences[address + 1], triangleReferences[address + 2]));
                        float3 maximum = asfloat(uint3(triangleReferences[address + 3], triangleReferences[address + 4], triangleReferences[address + 5]));
                        uint escape = triangleReferences[address + 6], count = triangleReferences[address + 7];
                        if (!VXGISDFRayCandidateBounds(localOrigin, localDirection, minimum - epsilon, maximum + epsilon,
                            current - epsilon, cellExit + epsilon))
                        {
                            node = escape;
                            continue;
                        }
                        if (count == 0) node += 8;
                        else { leafCursor = node + 8; leafEnd = leafCursor + count; node = escape; }
                    }
                    if (leafCursor == leafEnd) break;
                    localTriangleIndex = triangleReferences[asset.referenceOffset + leafCursor++];
                }
                else
                {
                    if (linearCursor >= linearEnd) break;
                    localTriangleIndex = triangleReferences[asset.referenceOffset + linearCursor++];
                }
                uint triangleIndex = asset.triangleOffset + localTriangleIndex;
                VXGISDFTriangle surfaceTriangle = triangles[triangleIndex];
                // Leaves may contain several triangles. Reject only their
                // conservative local boxes before transforming vertices and
                // running the exact world test. Include the same barycentric
                // fringe as the packed BVH plus local-ray arithmetic error.
                float3 triangleScale = max(abs(surfaceTriangle.p0), max(abs(surfaceTriangle.p1), abs(surfaceTriangle.p2)));
                float3 triangleMargin = 4e-5 * (abs(surfaceTriangle.p1 - surfaceTriangle.p0) + abs(surfaceTriangle.p2 - surfaceTriangle.p0))
                    + 4.0 * 1.1920928955078125e-7 * triangleScale + asset.cellSize * 1e-5 + epsilon;
                float3 triangleMinimum = min(surfaceTriangle.p0, min(surfaceTriangle.p1, surfaceTriangle.p2)) - triangleMargin;
                float3 triangleMaximum = max(surfaceTriangle.p0, max(surfaceTriangle.p1, surfaceTriangle.p2)) + triangleMargin;
                if (!VXGISDFRayCandidateBounds(localOrigin, localDirection, triangleMinimum, triangleMaximum,
                    current - epsilon, cellExit + epsilon)) continue;
                double worldDistance;
                float3 barycentric, worldNormal;
                if (!VXGIMeshSDFWorldTriangle(rayOrigin, rayDirection, surfaceTriangle, instance,
                    double(rayMin), closestWorld, worldDistance, barycentric, worldNormal)) continue;
                double localDistance = worldDistance * double(distanceScale);
                if (localDistance < double(current - epsilon) || localDistance > double(cellExit + epsilon)) continue;
#ifdef VXGI_SDF_TRACE_COVERAGE
                surfaceTriangle.flags = instance.flags;
                surfaceTriangle.materialId = surfaceTriangle.pad0 < instance.materialCount
                    ? VXGIMeshSDFMaterials[instance.materialOffset + surfaceTriangle.pad0] : 0xffffffffu;
                uint coverage = VXGISDFCandidateCoverage(surfaceTriangle, barycentric,
                    materials, materialCount, materialSampler, materialMipLevel);
                if (coverage == 2)
                {
                    hit.valid = 0;
                    hit.traceStatus = 2;
                    return false;
                }
                if (coverage == 0) continue;
#endif
                bool nearer = !found || worldDistance < closestWorld;
                if (found && worldDistance == closestWorld)
                {
                    uint previousInstanceId = VXGIMeshSDFInstances[hit.instanceIndex].instanceId;
                    nearer = instance.instanceId < previousInstanceId
                        || (instance.instanceId == previousInstanceId && triangleIndex < hit.triangleIndex);
                }
                if (!nearer) continue;
                found = true;
                closestWorld = worldDistance;
                hit.distance = float(worldDistance);
                hit.position = float3(double3(rayOrigin) + double3(rayDirection) * worldDistance);
                hit.geometricNormal = worldNormal;
                hit.triangleId = surfaceTriangle.triangleId;
                hit.triangleIndex = triangleIndex;
                hit.instanceIndex = instanceIndex;
                hit.barycentric = barycentric;
                hit.backFace = dot(rayDirection, worldNormal) > 0.0 ? 1u : 0u;
                hit.valid = 1;
                hit.traceStatus = 1;
            }
            if (cellExit >= exit)
            {
                completed = true;
                break;
            }
            float safeStep = 0.9 * max(0.0,
                VXGIMeshSDFConservativeDistance(localOrigin + localDirection * current, asset, nodes) - epsilon);
            if (current + safeStep > cellExit + epsilon)
            {
                current += safeStep;
                VXGISDFInitializeDDA(localOrigin, localDirection, current, localGrid, epsilon,
                    cell, step, boundaryDistance, deltaDistance);
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
        if (!completed)
        {
            hit.valid = 0;
            hit.traceStatus = 3;
            return false;
        }
    }
    return found;
