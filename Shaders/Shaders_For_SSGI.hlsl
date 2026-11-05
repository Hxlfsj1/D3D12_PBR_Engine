// Stages retain independent b0 layouts; select one with SSGI_PASS_<STAGE>.

#if defined(SSGI_PASS_PYRAMID)

#include "SSGICommon.hlsli"

cbuffer SSGIPyramidConstants : register(b0)
{
    float4x4 invProjMat;
    uint2 sourceSize;
    uint2 outputSize;
    uint depthTextureIdx;
    uint outputDepthTextureIdx;
    uint firstLevel;
    uint padding;
};

// Bounds are positive view Z: (nearest, furthest). Empty cells use (1e20, 0).
// Each level is a separate resource so RDG needs no subresource state tracking.

[numthreads(8, 8, 1)]
void CSMain_Pyramid(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= outputSize))
    {
        return;
    }
    Texture2D<float2> depths = ResourceDescriptorHeap[depthTextureIdx];
    RWTexture2D<float2> outputDepth = ResourceDescriptorHeap[outputDepthTextureIdx];
    float2 bounds = float2(1e20, 0);

    [unroll]
    for (uint i = 0; i < 4; ++i)
    {
        uint2 p = id.xy * 2 + uint2(i & 1, i >> 1);
        float2 b = float2(1e20, 0);
        if (all(p < sourceSize))
        {
            b = depths.Load(int3(p, 0));
            if (firstLevel)
            {
                float z = ReconstructPosition((p + 0.5) / sourceSize, b.x, invProjMat).z;
                b = b.x < 1.0 ? z.xx : float2(1e20, 0);
            }
        }
        bounds = float2(min(bounds.x, b.x), max(bounds.y, b.y));
    }
    outputDepth[id.xy] = bounds;
}

#elif defined(SSGI_PASS_TRACE)

#include "SSGICommon.hlsli"
#include "BlueNoise.hlsli"
#include "SSGIHybridCommon.hlsli"

cbuffer SSGITraceConstants : register(b0)
{
    float4x4 viewMat;
    float4x4 projMat;
    float4x4 invProjMat;
    uint2 sceneSize;
    uint2 outputSize;
    uint depthTextureIdx;
    uint normalTextureIdx;
    uint ormTextureIdx;
    uint sourceTextureIdx;
    uint outputTextureIdx;
    uint levelCount;
    uint frameIndex;
    uint blueNoiseTextureIdx;
    float radius;
    float thickness;
    float normalBias;
    float padding2;
    uint rayCount;
    uint maxIterations;
    uint albedoTextureIdx;
    uint traceDownsampleFactor;
    uint4 levels[7]; // Keep in sync with SSGIMaxLevels.
    float4x4 invViewMat;
    float4x4 invViewProjMat;
    SSGIHybridParameters hybrid;
    float4x4 prevJitteredViewProj;
    float4x4 prevJitteredInvViewProj;
    uint previousGITextureIdx;
    uint previousDepthTextureIdx;
    uint previousNormalTextureIdx;
    uint historyValid;
    uint probeNeighborsIndex; // Optional six-neighbor SRV; checkerboarding remains fixed.
    uint previousFastTextureIdx;
    uint reprojectedGITextureIdx;
    uint reprojectedFastTextureIdx;
#ifdef VXGI_SSGI_GLOBAL
    SSGIGlobalParameters global;
#endif
    uint worldSharingEnabled;
    uint sharedMeshLightingTextureIdx;
    uint sharedMeshReceiverTextureIdx;
    uint sharedGlobalLightingTextureIdx;
    uint sharedGlobalReceiverTextureIdx;
    uint sharedRepairQueueTextureIdx;
    uint sharedRepairCountTextureIdx;
    uint sharedMeshSpan;
    uint previousMeshBandIndex;
    uint previousGlobalBandIndex;
    uint currentMeshBandIndex;
    uint currentGlobalBandIndex;
};

// These uint4 components are reserved by the depth-pyramid ABI. Keep b0 at
// 1024 bytes and the world-mode/probe-neighbor aliases at their existing offsets.
#define previousScreenBandTextureIdx levels[0].y
#define currentScreenBandTextureIdx levels[0].z
#define sharedScreenLightingTextureIdx levels[0].w
#define sharedScreenReceiverTextureIdx levels[1].y
#define previousWorldPolicyTextureIdx levels[1].z
#define currentWorldPolicyTextureIdx levels[1].w
#define worldNearRayCount levels[2].w
#define worldFarRayCount levels[3].y

StructuredBuffer<float> hybridNodes : register(t0);
#ifndef VXGI_SSGI_THRESHOLD
StructuredBuffer<uint> hybridOffsets : register(t1);
StructuredBuffer<uint> hybridReferences : register(t2);
StructuredBuffer<VXGISDFTriangle> hybridTriangles : register(t3);
StructuredBuffer<VXGIMaterialData> hybridMaterials : register(t4);
StructuredBuffer<float3> hybridCenters : register(t5);
#endif
StructuredBuffer<VXGIProbeCellEntry> hybridLookup : register(t6);
StructuredBuffer<VXGIProbeRecord> hybridRawRecords : register(t7);
#ifndef VXGI_SSGI_THRESHOLD
SamplerState hybridMaterialSampler : register(s0);
#endif

#ifdef SSGI_MODE_FROM_SRV
ByteAddressBuffer traceModeConstants : register(t4, space1);
#endif

uint SSGIWorldMode()
{
#ifdef SSGI_MODE_FROM_SRV
    // Temporary compromise, not a proven optimal design: read only worldMode
    // through an SRV alias to avoid the observed CBV startup slow state
    // (~3.5 ms vs ~0.75 ms Trace on the tested NVIDIA setup, with matching output).
    // The root cause remains unconfirmed; retain this until a measured fix exists.
    // Same allocation as b0; the byte offset is checked against the CPU layout.
    return traceModeConstants.Load(604);
#else
    return hybrid.worldMode;
#endif
}

float3 Project(float3 p)
{
    float4 clip = mul(float4(p, 1), projMat);
    return float3(clip.xy / clip.w * float2(0.5, -0.5) + 0.5, clip.z / clip.w);
}

float3 CosineDirection(float2 xi, float3 n)
{
    float phi = 6.28318530718 * xi.y;
    float3 tangent = normalize(cross(abs(n.z) < 0.999 ? float3(0, 0, 1) : float3(0, 1, 0), n));
    return tangent * (sqrt(xi.x) * cos(phi)) + cross(n, tangent) * (sqrt(xi.x) * sin(phi)) + n * sqrt(1 - xi.x);
}

// The hierarchy stores positive view-Z bounds, while traversal uses projected device Z.
float DeviceDepth(float viewZ)
{
    float4 clip = mul(float4(0, 0, viewZ, 1), projMat);
    return clip.z / clip.w;
}

// Select the cell just ahead of a boundary. Offset only the address, not the ray segment.
int2 RayCell(float2 positionPixels, float2 directionPixels, uint mip)
{
    return int2(floor((positionPixels + sign(directionPixels) * 1e-4) / float(1u << mip)));
}

float CellExit(float2 startPixels, float2 deltaPixels, int2 cell, uint mip, float endT)
{
    float2 boundary = (float2(cell) + float2(deltaPixels.x > 0, deltaPixels.y > 0)) * float(1u << mip);
    float tx = abs(deltaPixels.x) > 1e-8 ? (boundary.x - startPixels.x) / deltaPixels.x : endT;
    float ty = abs(deltaPixels.y) > 1e-8 ? (boundary.y - startPixels.y) / deltaPixels.y : endT;
    return min(endT, min(tx, ty));
}

// Intersect the WHOLE segment inside a cell with its conservative depth slab.
// This also handles rays travelling towards the camera and rays parallel to the depth plane.
bool DepthInterval(float startZ, float deltaZ, float2 bounds, float t, float exitT, out float entryT)
{
    float nearDepth = DeviceDepth(bounds.x);
    float farDepth = DeviceDepth(bounds.y + thickness);
    entryT = t;
    if (abs(deltaZ) < 1e-8)
    {
        return startZ >= nearDepth && startZ <= farDepth;
    }
    float a = (nearDepth - startZ) / deltaZ;
    float b = (farDepth - startZ) / deltaZ;
    entryT = max(t, min(a, b));
    return entryT <= min(exitT, max(a, b));
}

// This is the finite segment already used by screen tracing. Viewport clipping
// only limits the screen traversal; it never changes this segment's 3D extent.
float SSGIViewRayLength(float3 origin, float3 direction)
{
    float rayLength = radius;
    float nearZ = ReconstructPosition(0.5.xx, 0, invProjMat).z;
    if (direction.z < 0)
    {
        rayLength = min(rayLength, max(0, (origin.z - nearZ * 1.01) / -direction.z));
    }
    return rayLength;
}

float3 SSGIReceiverGeometricNormal(uint2 receiver, float3 shadingNormal)
{
    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    int2 p = int2(receiver);
    int2 last = int2(sceneSize) - 1;
    int2 left = max(p - int2(1, 0), 0);
    int2 right = min(p + int2(1, 0), last);
    int2 up = max(p - int2(0, 1), 0);
    int2 down = min(p + int2(0, 1), last);
    float z = depth.Load(int3(p, 0));
    float zl = depth.Load(int3(left, 0));
    float zr = depth.Load(int3(right, 0));
    float zu = depth.Load(int3(up, 0));
    float zd = depth.Load(int3(down, 0));
    // Prefer the neighbor on the same depth surface at object silhouettes.
    float dl = zl < 1.0 && left.x != p.x ? abs(zl - z) : 1e20;
    float dr = zr < 1.0 && right.x != p.x ? abs(zr - z) : 1e20;
    float du = zu < 1.0 && up.y != p.y ? abs(zu - z) : 1e20;
    float dd = zd < 1.0 && down.y != p.y ? abs(zd - z) : 1e20;
    if (min(dl, dr) == 1e20 || min(du, dd) == 1e20) return shadingNormal;
    bool useRight = dr <= dl;
    bool useDown = dd <= du;
    int2 px = useRight ? right : left;
    int2 py = useDown ? down : up;
    float3 center = ReconstructPosition((p + 0.5) / sceneSize, z, invViewProjMat);
    float3 x = ReconstructPosition((px + 0.5) / sceneSize, useRight ? zr : zl, invViewProjMat);
    float3 y = ReconstructPosition((py + 0.5) / sceneSize, useDown ? zd : zu, invViewProjMat);
    float3 dx = useRight ? x - center : center - x;
    float3 dy = useDown ? y - center : center - y;
    float3 geometricNormal;
    if (!SSGINormalizeDirection(cross(dx, dy), geometricNormal)) return shadingNormal;
    return dot(geometricNormal, shadingNormal) < 0.0 ? -geometricNormal : geometricNormal;
}

// Screen tracing retains its small offset. Only its world fallback moves the
// launch point outside the local SDF threshold shell. Cache the point query per
// receiver, lazily on the first screen miss, and reuse it for the other rays.
bool SSGIApplyWorldNormalBias(uint2 receiver, inout float3 worldOrigin,
    inout float worldRayLength, inout float4 cachedWorldOffset)
{
#ifdef VXGI_SSGI_THRESHOLD
    if (cachedWorldOffset.w < 0.0)
    {
        float worldBias = SSGIWorldReceiverNormalBias(worldOrigin, hybrid, normalBias);
        cachedWorldOffset = 0.0;
        if (worldBias > normalBias)
        {
            Texture2D<float4> normals = ResourceDescriptorHeap[normalTextureIdx];
            float3 shadingNormal;
            if (!SSGINormalizeDirection(DecodeGBufferNormal(normals.Load(int3(receiver, 0)).xyz), shadingNormal))
            {
                cachedWorldOffset.w = -1.0;
                return false;
            }
            float3 geometricNormal = SSGIReceiverGeometricNormal(receiver, shadingNormal);
            // Replace the screen's shading-normal offset, rather than stacking
            // another offset onto it. Normal-map bumps must not aim the launch.
            cachedWorldOffset.xyz = geometricNormal * worldBias - shadingNormal * normalBias;
            cachedWorldOffset.w = length(cachedWorldOffset.xyz);
        }
    }
    if (cachedWorldOffset.w == 0.0) return true;
    // Spend the offset out of both ranges. A consumed near segment still has
    // a valid far continuation when Global SDF is enabled.
    if (cachedWorldOffset.w >= worldRayLength)
    {
#ifdef VXGI_SSGI_GLOBAL
        if (global.enabled != 0)
        {
            worldOrigin += cachedWorldOffset.xyz;
            worldRayLength = 0.0;
            return true;
        }
#endif
        return false;
    }
    worldOrigin += cachedWorldOffset.xyz;
    worldRayLength -= cachedWorldOffset.w;
#endif
    return true;
}

// Stackless hierarchical grid traversal, ordinary Z. Mip 0 is the full-resolution depth;
// mip 1..LevelCount map to Levels[0..LevelCount-1] (2x2 through 128x128 pixel cells).
SSGIScreenRayResult TraceScreenRay(uint2 receiver, float3 origin, float3 direction)
{
    SSGIScreenRayResult result = (SSGIScreenRayResult)0;
    result.status = SSGI_SCREEN_INVALID;
    if (any(sceneSize == 0) || any(receiver >= sceneSize) || !all(isfinite(origin))
        || !all(isfinite(direction)) || dot(direction, direction) <= 0.0
        || !isfinite(radius) || radius <= 0.0 || levelCount > 7) return result;
    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    Texture2D<float4> normals = ResourceDescriptorHeap[normalTextureIdx];
    float rayLength = SSGIViewRayLength(origin, direction);
    float3 start = Project(origin);
    float3 delta = Project(origin + direction * rayLength) - start;
    if (!all(isfinite(start)) || !all(isfinite(delta)) || !isfinite(rayLength)) return result;
    float endT = 1;

    [unroll]
    for (uint axis = 0; axis < 2; ++axis)
    {
        if (delta[axis] > 0)
        {
            endT = min(endT, (1 - start[axis]) / delta[axis]);
        }
        if (delta[axis] < 0)
        {
            endT = min(endT, -start[axis] / delta[axis]);
        }
    }
    if (any(start.xy < 0) || any(start.xy >= 1) || endT <= 0)
    {
        result.status = SSGI_SCREEN_OUT_OF_SCREEN;
        return result;
    }

    float2 startPixels = start.xy * sceneSize;
    float2 deltaPixels = delta.xy * sceneSize;
    // As in UE's Lumen traversal, leave the starting fine cell without testing itself.
    float t = CellExit(startPixels, deltaPixels, RayCell(startPixels, deltaPixels, 0), 0, endT);
    uint mip = 0;
    uint iteration = 0;

    [loop]
    for (; iteration < maxIterations && t < endT; ++iteration)
    {
        int2 cell = RayCell(startPixels + deltaPixels * t, deltaPixels, mip);
        uint cellSize = 1u << mip;
        int2 gridSize = int2((sceneSize + cellSize - 1) / cellSize);
        if (any(cell < 0) || any(cell >= gridSize))
        {
            result.status = SSGI_SCREEN_OUT_OF_SCREEN;
            return result;
        }
        float exitT = CellExit(startPixels, deltaPixels, cell, mip, endT);
        float2 bounds;
        if (mip == 0)
        {
            float z = depth.Load(int3(cell, 0));
            float viewZ = ReconstructPosition((cell + 0.5) / sceneSize, z, invProjMat).z;
            bounds = z < 1 ? viewZ.xx : float2(1e20, 0);
        }
        else
        {
            // Each lane can be at a different mip after adaptive traversal.
            Texture2D<float2> hierarchy = ResourceDescriptorHeap[NonUniformResourceIndex(levels[mip - 1].x)];
            // Physical cell size, not UV * ceil(mipSize), keeps odd image edges aligned.
            bounds = hierarchy.Load(int3(cell, 0));
        }

        float entryT = t;
        bool potentialHit = false;
        if (bounds.x <= bounds.y)
        {
            potentialHit = DepthInterval(start.z, delta.z, bounds, t, exitT, entryT);
        }
        if (potentialHit && mip > 0)
        {
            // No hit can precede entryT in this cell. Descend at that exact ray position.
            t = entryT;
            --mip;
            continue;
        }
        if (potentialHit && any(uint2(cell) != receiver))
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

        // The current cell has been ruled out. Cross its boundary and try a coarser cell.
        t = exitT;
        mip = min(mip + 1, levelCount);
    }
    result.status = iteration >= maxIterations && t < endT ? SSGI_SCREEN_ITERATION_LIMIT
        : endT < 1.0 ? SSGI_SCREEN_OUT_OF_SCREEN : SSGI_SCREEN_RADIUS_LIMIT;
    return result;
}

// The fixed distance bands share the original ray origin. A farther band
// first traces every nearer interval for visibility; its lower bound only
// controls contribution, never which occluding surfaces are skipped.
SSGIHybridRayResult TraceBandMesh(float3 origin, float3 direction, float distanceLimit)
{
#ifdef VXGI_SSGI_GLOBAL
    SSGIGlobalParameters meshOnly = global;
    meshOnly.enabled = 0;
#endif
    return SSGIWorldRaySource(origin, direction, distanceLimit, hybrid, hybridNodes,
#ifndef VXGI_SSGI_THRESHOLD
        hybridOffsets, hybridReferences, hybridTriangles, hybridMaterials,
        hybridLookup, hybridRawRecords, hybridMaterialSampler
#else
        hybridLookup, hybridRawRecords
#endif
#ifdef VXGI_SSGI_GLOBAL
        , meshOnly
#endif
        , probeNeighborsIndex
        );
}

float4 TraceDistanceBand(uint2 receiver, float3 viewOrigin, float3 viewDirection,
    uint band, inout float4 cachedWorldOffset)
{
    SSGIScreenRayResult screen = TraceScreenRay(receiver, viewOrigin, viewDirection);
    if (screen.status == SSGI_SCREEN_HIT)
    {
        // Only the fixed screen estimate owns screen radiance. World-only
        // directions still test this segment, but its hits only block them.
        if (band != 0u) return 0;
        Texture2D<float4> source = ResourceDescriptorHeap[sourceTextureIdx];
        float3 radiance = source.Load(int3(screen.hitPixel, 0)).rgb;
        return float4(all(isfinite(radiance)) ? radiance : 0, 1);
    }
    if (band == 0u || SSGIWorldMode() == 0) return 0;
    float3 worldOrigin = mul(float4(viewOrigin, 1), invViewMat).xyz;
    float3 worldDirection;
    if (!SSGINormalizeDirection(mul(float4(viewDirection, 0), invViewMat).xyz, worldDirection)) return 0;
    float limit = 2.0;
    if (!SSGIApplyWorldNormalBias(receiver, worldOrigin, limit, cachedWorldOffset)) return 0;
    float spent = max(cachedWorldOffset.w, 0.0);
    SSGIHybridRayResult hit = (SSGIHybridRayResult)0;
    if (limit > 0) hit = TraceBandMesh(worldOrigin, worldDirection, limit);
    if (hit.hit != 0)
    {
        float pathDistance = hit.distance + spent;
        bool belongs = band == 1u && pathDistance <= 2.0;
        return belongs ? float4(hit.radiance, 1) : 0;
    }
#if defined(VXGI_SSGI_THRESHOLD) && defined(VXGI_SSGI_GLOBAL)
    if (band != 2u || hit.worldTraceStatus != VXGI_THRESHOLD_MISS) return 0;
    if (global.enabled != 0)
    {
        SSGIGlobalParameters farParameters = global;
        farParameters.maxTraceDistance = max(0.0, 8.0 - spent);
        SSGIHybridRayResult farHit = SSGIGlobalRaySource(worldOrigin, worldDirection,
            limit, hybrid, hybridNodes, hybridLookup, hybridRawRecords, farParameters, probeNeighborsIndex);
        float pathDistance = farHit.distance + spent;
        if (farHit.hit != 0 && pathDistance > 2.0 && pathDistance <= 8.0)
            return float4(farHit.radiance, 1);
    }
#endif
    return 0;
}

// RGB is the independent distance-band estimate. Alpha is its integer age,
// not the final raw-GI hit fraction. Conservative max age prevents interpolation
// between tiles from indefinitely postponing a required refresh.
// period=0 only transports history for a checkerboard-skipped, nonshared pixel;
// it advances age without pretending an expired sample has been refreshed.
bool ReprojectDistanceBand(uint2 receiverGI, uint previousIndex, uint period, out float4 value)
{
    value = float4(0, 0, 0, -1);
    if (historyValid == 0) return false;
    uint factor = SSGITraceFactor(traceDownsampleFactor);
    uint2 pixel = receiverGI * factor;
    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    Texture2D<float4> normals = ResourceDescriptorHeap[normalTextureIdx];
    float z = depth.Load(int3(pixel, 0));
    float2 uv = (pixel + 0.5) / sceneSize;
    float3 p = ReconstructPosition(uv, z, invViewProjMat);
    float3 n = normalize(DecodeGBufferNormal(normals.Load(int3(pixel, 0)).xyz));
    float footprint = max(SameDepthPixelFootprint(uv, z, p, sceneSize, invViewProjMat), 1e-4);
    float rayDepth = length(p - ReconstructPosition(uv, 0, invViewProjMat));
    float tolerance = max(min(footprint * 0.5, rayDepth * 0.01), 0.001);
    float4 clip = mul(float4(p, 1), prevJitteredViewProj);
    if (clip.w <= 0) return false;
    float3 ndc = clip.xyz / clip.w;
    float2 previousUV = ndc.xy * float2(0.5, -0.5) + 0.5;
    if (any(previousUV < 0) || any(previousUV >= 1) || ndc.z < 0 || ndc.z >= 1) return false;
    float2 historyPixel = (previousUV * sceneSize - 0.5) / float(factor);
    int2 base = int2(floor(historyPixel));
    float2 fraction = frac(historyPixel);
    Texture2D<float4> previousBand = ResourceDescriptorHeap[previousIndex];
    Texture2D<float> previousDepth = ResourceDescriptorHeap[previousDepthTextureIdx];
    Texture2D<float4> previousNormals = ResourceDescriptorHeap[previousNormalTextureIdx];
    float3 sum = 0;
    float weightSum = 0, maxAge = 0;
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        int2 offset = int2(i & 1u, i >> 1u);
        int2 tap = base + offset;
        if (any(tap < 0) || any(tap >= int2(outputSize))) continue;
        float2 weights = lerp(1 - fraction, fraction, float2(offset));
        float weight = weights.x * weights.y;
        if (weight < 1e-6) continue;
        float4 normal = previousNormals.Load(int3(tap, 0));
        if (normal.a < 0.5) continue;
        float previousZ = previousDepth.Load(int3(tap, 0));
        float3 q = ReconstructPosition((tap * int(factor) + 0.5) / sceneSize, previousZ, prevJitteredInvViewProj);
        if (!SSGISameSurface(p, n, q, normalize(DecodeGBufferNormal(normal.xyz)), tolerance)) continue;
        float4 cache = previousBand.Load(int3(tap, 0));
        if (!all(isfinite(cache)) || cache.a < 0 ||
            (period != 0u && cache.a >= float(period - 1u))) return false;
        sum += cache.rgb * weight;
        weightSum += weight;
        maxAge = max(maxAge, cache.a);
    }
    if (weightSum < 0.01) return false;
    value = float4(sum / weightSum, maxAge + 1.0);
    return true;
}

uint SSGIWorldRayCount(uint span)
{
    uint count = span == 4u ? worldFarRayCount : worldNearRayCount;
    // Older diagnostic callers leave these reserved slots zero. Production
    // validates configuration on the CPU; keep legacy calls safe at four.
    return count >= 4u && (count & 3u) == 0u ? count : 4u;
}

uint SSGIRaySampleIndex(uint baseIndex, uint ray)
{
    // Keep the original first 16 near/far directions, then use disjoint blocks
    // for larger point-Repair budgets rather than overlapping the other band.
    // This is a procedural pattern ID, not a resource address. Unsigned wrap
    // is allowed for extreme budgets; LoadSTBN2D bounds the final texture lookup.
    return baseIndex + (ray & 15u) + 48u * (ray >> 4u);
}

float4 TraceReceiverBand(uint2 receiverGI, uint band, uint samples)
{
    uint factor = SSGITraceFactor(traceDownsampleFactor);
    uint2 receiver = receiverGI * factor;
    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    Texture2D<float4> normals = ResourceDescriptorHeap[normalTextureIdx];
    Texture2DArray<uint2> blueNoise = ResourceDescriptorHeap[blueNoiseTextureIdx];
    float z = depth.Load(int3(receiver, 0));
    float3 position = ReconstructPosition((receiver + 0.5) / sceneSize, z, invProjMat);
    float3 n = SSGIDecodeViewNormal(normals.Load(int3(receiver, 0)).xyz, viewMat);
    float4 sum = 0;
    float4 cachedWorldOffset = float4(0, 0, 0, -1);
    [loop] for (uint ray = 0; ray < samples; ++ray)
    {
        uint sampleIndex = SSGIRaySampleIndex(band == 0u ? 16u : band * 16u, ray);
        float3 direction = CosineDirection(LoadSTBN2D(blueNoise, receiverGI, frameIndex, sampleIndex), n);
        sum += TraceDistanceBand(receiver, position + n * normalBias, direction, band, cachedWorldOffset);
    }
    return sum / float(samples);
}

#include "SSGICameraQuality.hlsli"

#if defined(VXGI_SSGI_THRESHOLD) && defined(VXGI_SSGI_GLOBAL)
// Reuse only a genuinely identical origin/direction. The fixed screen signal
// and the two world signals retain independent update and normalization rules.
void TraceReceiverDirectionBands(uint2 receiverGI, uint2 receiver, float3 position, float3 n,
    Texture2DArray<uint2> blueNoise, bool needScreen, bool needMesh, bool needFar,
    uint sampleOffset, inout float4 cachedWorldOffset,
    out float3 screenContribution, out float3 meshContribution, out float3 farContribution)
{
    screenContribution = 0;
    meshContribution = 0;
    farContribution = 0;
    // Preserve the fixed screen/near sample and the far-only sequence. Only
    // requested observations share this prefix. World budgets are independent
    // of the fixed four screen observations.
    uint sampleIndex = SSGIRaySampleIndex((needScreen || needMesh) ? 16u : 32u, sampleOffset);
    float3 viewDirection = CosineDirection(LoadSTBN2D(blueNoise, receiverGI, frameIndex, sampleIndex), n);
    float3 viewOrigin = position + n * normalBias;
    SSGIScreenRayResult screen = TraceScreenRay(receiver, viewOrigin, viewDirection);
    if (screen.status == SSGI_SCREEN_HIT)
    {
        if (needScreen)
        {
            Texture2D<float4> source = ResourceDescriptorHeap[sourceTextureIdx];
            float3 radiance = source.Load(int3(screen.hitPixel, 0)).rgb;
            screenContribution = all(isfinite(radiance)) ? radiance : 0;
        }
        return; // Black/unsupported near surfaces still occlude the far band.
    }
    if ((!needMesh && !needFar) || SSGIWorldMode() == 0) return;
    float3 worldOrigin = mul(float4(viewOrigin, 1), invViewMat).xyz;
    float3 worldDirection;
    if (!SSGINormalizeDirection(mul(float4(viewDirection, 0), invViewMat).xyz, worldDirection)) return;
    float limit = 2.0;
    if (!SSGIApplyWorldNormalBias(receiver, worldOrigin, limit, cachedWorldOffset)) return;
    float spent = max(cachedWorldOffset.w, 0.0);
    // Match SSGIWorldRaySource's second normalization before threshold tracing
    // and its SH evaluation direction. Keep worldDirection for the Global call.
    float3 unitDirection;
    if (!SSGINormalizeDirection(worldDirection, unitDirection)) return;
    VXGIMeshSDFThresholdCounts counts;
    counts.nodeCount = hybrid.nodeCount;
    counts.assetCount = hybrid.assetCount;
    counts.instanceCount = hybrid.instanceCount;
    counts.bvhNodeCount = hybrid.bvhNodeCount;
    VXGIThresholdParameters parameters;
    parameters.hitThresholdCells = SSGI_WORLD_HIT_THRESHOLD_CELLS;
    parameters.stepScale = 1.0; // Use the full conservative distance lower bound.
    parameters.maxSteps = 512;
    parameters.padding = 0;
    // An offset that consumes the short interval leaves a completed empty
    // near segment, preserving the original far continuation behavior.
    VXGIMeshSDFThresholdResult hit = (VXGIMeshSDFThresholdResult)0;
    if (limit > 0)
        hit = SSGITraceMeshNear(worldOrigin, unitDirection, 0.0, limit,
            counts, parameters, hybridNodes, hybrid);
    if (hit.status == VXGI_THRESHOLD_HIT)
    {
        if (needMesh && hit.distance + spent <= 2.0)
        {
            VXGIProbeRadianceQueryResult query = VXGIProbeQueryRadiance(hit.position,
                unitDirection, hybrid.voxelSize, hybrid.probeCount, hybridLookup,
                hybridRawRecords, probeNeighborsIndex, asuint(hybrid.reservedLighting0.x));
            if (query.hasSupport != 0 && all(isfinite(query.radiance)))
                meshContribution = query.radiance;
        }
        return;
    }
    if (!needFar || global.enabled == 0 || hit.status != VXGI_THRESHOLD_MISS) return;
    SSGIGlobalParameters farParameters = global;
    farParameters.maxTraceDistance = max(0.0, 8.0 - spent);
    SSGIHybridRayResult farHit = SSGIGlobalRaySource(worldOrigin, worldDirection,
        limit, hybrid, hybridNodes, hybridLookup, hybridRawRecords, farParameters, probeNeighborsIndex);
    float pathDistance = farHit.distance + spent;
    if (farHit.hit != 0 && pathDistance > 2.0 && pathDistance <= 8.0)
        farContribution = farHit.radiance;
}

void TraceSharedReceiverBands(uint2 receiverGI, bool needScreen, bool needMesh, bool needFar,
    uint worldSamples,
    out float3 screenContribution, out float3 meshContribution, out float3 farContribution)
{
    uint2 receiver = receiverGI * SSGITraceFactor(traceDownsampleFactor);
    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    Texture2D<float4> normals = ResourceDescriptorHeap[normalTextureIdx];
    Texture2DArray<uint2> blueNoise = ResourceDescriptorHeap[blueNoiseTextureIdx];
    float z = depth.Load(int3(receiver, 0));
    float3 position = ReconstructPosition((receiver + 0.5) / sceneSize, z, invProjMat);
    float3 n = SSGIDecodeViewNormal(normals.Load(int3(receiver, 0)).xyz, viewMat);
    float4 cachedWorldOffset = float4(0, 0, 0, -1);
    screenContribution = 0;
    meshContribution = 0;
    farContribution = 0;
    uint sampleCount = max(needScreen ? 1u : 0u, (needMesh || needFar) ? worldSamples : 0u);
    [loop] for (uint ray = 0; ray < sampleCount; ++ray)
    {
        float3 screen, mesh, farValue;
        TraceReceiverDirectionBands(receiverGI, receiver, position, n, blueNoise,
            needScreen && ray == 0u, needMesh, needFar, ray, cachedWorldOffset,
            screen, mesh, farValue);
        screenContribution += screen;
        meshContribution += mesh;
        farContribution += farValue;
    }
    // Screen has one observation at this origin; each world stratum averages
    // its own directions before the original spatial member weights apply.
    meshContribution /= float(worldSamples);
    farContribution /= float(worldSamples);
}
#endif

void EvaluateReceiverNearBands(uint2 receiverGI, uint span, bool requestScreen, bool requestMesh,
    out float4 screenValue, out float4 meshValue, out uint meshKind)
{
    screenValue = float4(0, 0, 0, -1);
    meshValue = screenValue;
    meshKind = SSGI_WORLD_CACHE_INVALID;
    bool freshScreen = false, freshMesh = false;
    if (requestScreen)
        freshScreen = !ReprojectDistanceBand(receiverGI, previousScreenBandTextureIdx, 2u, screenValue);
    if (requestMesh)
        freshMesh = !ReprojectWorldBand(receiverGI, previousMeshBandIndex, 2u, span, true, meshValue, meshKind);
    if (freshScreen || freshMesh)
    {
#if defined(VXGI_SSGI_THRESHOLD) && defined(VXGI_SSGI_GLOBAL)
        // The original combined near estimate used one screen/Mesh query for
        // each index16+ray direction. Keep that sharing after separating caches.
        // Receiver preparation and the lazy world offset stay outside the loop.
        uint2 receiver = receiverGI * SSGITraceFactor(traceDownsampleFactor);
        Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
        Texture2D<float4> normals = ResourceDescriptorHeap[normalTextureIdx];
        Texture2DArray<uint2> blueNoise = ResourceDescriptorHeap[blueNoiseTextureIdx];
        float z = depth.Load(int3(receiver, 0));
        float3 position = ReconstructPosition((receiver + 0.5) / sceneSize, z, invProjMat);
        float3 n = SSGIDecodeViewNormal(normals.Load(int3(receiver, 0)).xyz, viewMat);
        float4 cachedWorldOffset = float4(0, 0, 0, -1);
        float3 screenSum = 0, meshSum = 0;
        uint worldSamples = SSGIWorldRayCount(span);
        uint sampleCount = max(freshScreen ? 4u : 0u, freshMesh ? worldSamples : 0u);
        [loop] for (uint ray = 0; ray < sampleCount; ++ray)
        {
            float3 screen, mesh, unusedFar;
            TraceReceiverDirectionBands(receiverGI, receiver, position, n, blueNoise,
                freshScreen && ray < 4u, freshMesh && ray < worldSamples, false,
                ray, cachedWorldOffset, screen, mesh, unusedFar);
            screenSum += screen;
            meshSum += mesh;
        }
        if (freshScreen) screenValue = float4(screenSum * 0.25, 0);
        if (freshMesh) meshValue = float4(meshSum / float(worldSamples), 0);
#else
        if (freshScreen) screenValue = float4(TraceReceiverBand(receiverGI, 0u, 4u).rgb, 0);
        if (freshMesh) meshValue = float4(TraceReceiverBand(receiverGI, 1u, SSGIWorldRayCount(span)).rgb, 0);
#endif
        if (freshMesh) meshKind = SSGI_WORLD_CACHE_POINT;
    }
}

uint2 SharedReceiverCoordinate(uint address)
{
    uint flat = (address & 0x7fffffffu) - 1u;
    return uint2(flat % outputSize.x, flat / outputSize.x);
}

#include "SSGIWorldOwnerCache.hlsli"

#if defined(SSGI_SHARED_WORLD_PASS)
groupshared uint bandValid[64];
groupshared uint screenAddresses[16];
groupshared uint bandMemberCounts[16];
groupshared uint worldParentSpans[4];
groupshared uint worldParentCounts[4];
groupshared uint worldParentReceivers[4];
groupshared uint worldSelectedLanes[16];
groupshared uint worldAddresses[16];
groupshared float4 cachedScreenBands[16];
groupshared float4 cachedMeshBands[16];
groupshared float2 cachedMeshState[16];
groupshared uint cachedMeshSources[16];
groupshared float4 cachedFarBands[16];
groupshared float4 oldMeshBatches[16];
groupshared float4 oldFarBatches[16];
groupshared float2 cachedFarState[16];
groupshared uint cachedFarSources[16];
groupshared float3 sharedScreenContributions[64];
groupshared float3 sharedMeshContributions[64];
groupshared float3 sharedFarContributions[64];

uint SelectBandTile(uint2 localBase, uint span, uint2 groupBase, out uint count)
{
    count = 0;
    uint selected = 64u;
    [loop] for (uint y = 0; y < span; ++y)
    [loop] for (uint x = 0; x < span; ++x)
    {
        uint lane = (localBase.y + y) * 8u + localBase.x + x;
        if (bandValid[lane] == 0u) continue;
        if (selected == 64u) selected = lane;
        ++count;
    }
    if (selected == 64u) return 0;
    uint2 selectedGI = groupBase + uint2(selected & 7u, selected >> 3u);
    return selectedGI.y * outputSize.x + selectedGI.x + 1u;
}

void PrepareWorldParent(uint2 local, uint2 groupBase, uint parent)
{
    uint span = 4u, total = 0u, first = 0u;
    [unroll] for (uint q = 0; q < 4; ++q)
    {
        uint2 base = local + uint2(q & 1u, q >> 1u) * 2u;
        uint members;
        uint address = SelectBandTile(base, 2u, groupBase, members);
        total += members;
        if (first == 0u) first = address;
        if (address != 0u &&
            (SSGIWorldMode() == 0u || SSGICameraSpan(SharedReceiverCoordinate(address)) == 2u))
            span = 2u;
    }
    worldParentSpans[parent] = span;
    worldParentCounts[parent] = total;
    worldParentReceivers[parent] = first;
}

uint SelectWorldStratumLane(uint2 base, uint2 groupBase, uint members)
{
    if (members == 0u) return 64u;
    Texture2DArray<uint2> noise = ResourceDescriptorHeap[blueNoiseTextureIdx];
    // Same STBN origin choice as before, now only evaluated for fresh coarse
    // world estimates. Fine layouts and cache hits do not need this selection.
    uint rank = min(uint(LoadSTBN2D(noise, groupBase + base, frameIndex, 48u).x * members), members - 1u);
    uint chosen = 64u;
    [unroll] for (uint y = 0; y < 2; ++y)
    [unroll] for (uint x = 0; x < 2; ++x)
    {
        uint member = (base.y + y) * 8u + base.x + x;
        if (bandValid[member] != 0u)
        {
            if (rank == 0u && chosen == 64u) chosen = member;
            else if (chosen == 64u) --rank;
        }
    }
    return chosen;
}

[numthreads(8, 8, 1)]
void CSMain_SharedWorld(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint2 local = uint2(lane & 7u, lane >> 3u);
    uint2 groupBase = group.xy * 8u;
    uint2 receiverGI = groupBase + local;
    uint2 receiver = receiverGI * SSGITraceFactor(traceDownsampleFactor);
    uint index = (local.y / 2u) * 4u + local.x / 2u;
    uint parent = (local.y / 4u) * 2u + local.x / 4u;
    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    Texture2D<float4> orm = ResourceDescriptorHeap[ormTextureIdx];
    bandValid[lane] = 0u;
    if (all(receiverGI < outputSize))
    {
        float z = depth.Load(int3(receiver, 0));
        if (isfinite(z) && z >= 0 && z < 1 && orm.Load(int3(receiver, 0)).a >= 0.5)
            bandValid[lane] = 1u;
    }
    GroupMemoryBarrierWithGroupSync();
    if (all((local & 1u) == 0u))
    {
        uint members;
        uint address = SelectBandTile(local, 2u, groupBase, members);
        screenAddresses[index] = address;
        bandMemberCounts[index] = members;
        worldSelectedLanes[index] = 64u;
        float4 value = float4(0, 0, 0, -1);
        if (address != 0u)
            ReprojectDistanceBand(SharedReceiverCoordinate(address), previousScreenBandTextureIdx, 2u, value);
        cachedScreenBands[index] = value;
    }
    if (all((local & 3u) == 0u)) PrepareWorldParent(local, groupBase, parent);
    GroupMemoryBarrierWithGroupSync();
    uint span = worldParentSpans[parent];
    if (all((local & 1u) == 0u) && (span == 2u || all((local & 3u) == 0u)))
    {
        uint address = span == 2u ? screenAddresses[index] : worldParentReceivers[parent];
        float4 mesh = float4(0, 0, 0, -1), farValue = mesh;
        float4 oldMesh = mesh, oldFar = farValue;
        float2 meshState = 0, farState = 0;
        uint meshSource = 0u, farSource = 0u;
        uint kind;
        if (address != 0u && SSGIWorldMode() != 0u)
        {
            uint2 origin = SharedReceiverCoordinate(address);
            if (SSGIWorldOwnerCacheEnabled())
            {
                SSGIReprojectWorldOwner(origin, span, 1u, mesh, meshState, meshSource);
                oldMesh = mesh;
                if (SSGIWorldOwnerRefreshDue(mesh, meshState, meshSource, 2u)) mesh = float4(0, 0, 0, -1);
            }
            else ReprojectWorldBand(origin, previousMeshBandIndex, 2u, span, false, mesh, kind);
#ifdef VXGI_SSGI_GLOBAL
            if (global.enabled != 0u)
            {
                if (SSGIWorldOwnerCacheEnabled())
                {
                    SSGIReprojectWorldOwner(origin, span, 2u, farValue, farState, farSource);
                    oldFar = farValue;
                    if (SSGIWorldOwnerRefreshDue(farValue, farState, farSource, 4u)) farValue = float4(0, 0, 0, -1);
                }
                else ReprojectWorldBand(origin, previousGlobalBandIndex, 4u, span, false, farValue, kind);
            }
#endif
        }
        bool freshWorld = mesh.a < 0;
#ifdef VXGI_SSGI_GLOBAL
        freshWorld = freshWorld || (global.enabled != 0u && farValue.a < 0);
#endif
        if (span == 4u && address != 0u && SSGIWorldMode() != 0u && freshWorld)
        {
            [unroll] for (uint q = 0; q < 4; ++q)
            {
                uint2 base = local + uint2(q & 1u, q >> 1u) * 2u;
                uint targetIndex = (base.y / 2u) * 4u + base.x / 2u;
                worldSelectedLanes[targetIndex] = SelectWorldStratumLane(base, groupBase, bandMemberCounts[targetIndex]);
            }
        }
        uint encoded = address == 0u ? 0u : SSGIWorldAddress(address, span);
        [unroll] for (uint q = 0; q < 4; ++q)
        {
            if (span == 2u && q != 0u) continue;
            uint2 target = local + uint2(q & 1u, q >> 1u) * 2u;
            uint targetIndex = (target.y / 2u) * 4u + target.x / 2u;
            worldAddresses[targetIndex] = encoded;
            cachedMeshBands[targetIndex] = mesh;
            oldMeshBatches[targetIndex] = oldMesh;
            oldFarBatches[targetIndex] = oldFar;
            cachedMeshState[targetIndex] = meshState;
            cachedMeshSources[targetIndex] = meshSource;
            cachedFarBands[targetIndex] = farValue;
            cachedFarState[targetIndex] = farState;
            cachedFarSources[targetIndex] = farSource;
        }
    }
    if (all(receiverGI < outputSize))
    {
        RWTexture2D<uint> policy = ResourceDescriptorHeap[currentWorldPolicyTextureIdx];
        policy[receiverGI] = bandValid[lane] != 0u ? SSGIPolicyForSpan(span) : 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    float3 screenContribution = 0, meshContribution = 0, farContribution = 0;
    if (bandValid[lane] != 0u)
    {
        bool selected = span == 2u || worldSelectedLanes[index] == lane;
        bool needScreen = screenAddresses[index] != 0u && cachedScreenBands[index].a < 0;
        bool needMesh = SSGIWorldMode() != 0u && selected && worldAddresses[index] != 0u && cachedMeshBands[index].a < 0;
        bool needFar = false;
#ifdef VXGI_SSGI_GLOBAL
        needFar = global.enabled != 0u && SSGIWorldMode() != 0u && selected &&
            worldAddresses[index] != 0u && cachedFarBands[index].a < 0;
#endif
#if defined(VXGI_SSGI_THRESHOLD) && defined(VXGI_SSGI_GLOBAL)
        if (needScreen || needMesh || needFar)
            TraceSharedReceiverBands(receiverGI, needScreen, needMesh, needFar,
                SSGIWorldRayCount(span) / 4u,
                screenContribution, meshContribution, farContribution);
#else
        if (needScreen) screenContribution = TraceReceiverBand(receiverGI, 0u, 1u).rgb;
        if (needMesh) meshContribution = TraceReceiverBand(receiverGI, 1u, SSGIWorldRayCount(span) / 4u).rgb;
        if (needFar) farContribution = TraceReceiverBand(receiverGI, 2u, SSGIWorldRayCount(span) / 4u).rgb;
#endif
        // A partial stratum represents its valid members, not one quarter of an
        // arbitrary rectangle. Black hits/misses remain zero observations.
        float weight = span == 2u ? rcp(float(max(bandMemberCounts[index], 1u))) :
            float(bandMemberCounts[index]) / float(max(worldParentCounts[parent], 1u));
        meshContribution *= weight;
        farContribution *= weight;
    }
    sharedScreenContributions[lane] = screenContribution;
    sharedMeshContributions[lane] = meshContribution;
    sharedFarContributions[lane] = farContribution;
    GroupMemoryBarrierWithGroupSync();
    if (all((local & 1u) == 0u))
    {
        uint2 tile = group.xy * 4u + local / 2u;
        uint2 size = (outputSize + 1u) / 2u;
        if (all(tile < size))
        {
            float4 value = cachedScreenBands[index];
            if (value.a < 0 && screenAddresses[index] != 0u)
            {
                float3 sum = 0;
                [unroll] for (uint y = 0; y < 2; ++y)
                [unroll] for (uint x = 0; x < 2; ++x)
                    sum += sharedScreenContributions[(local.y + y) * 8u + local.x + x];
                value = float4(sum / float(max(bandMemberCounts[index], 1u)), 0);
            }
            RWTexture2D<float4> lighting = ResourceDescriptorHeap[sharedScreenLightingTextureIdx];
            RWTexture2D<uint> addresses = ResourceDescriptorHeap[sharedScreenReceiverTextureIdx];
            lighting[tile] = value;
            addresses[tile] = screenAddresses[index];
        }
    }
    if (all((local & 1u) == 0u) && (span == 2u || all((local & 3u) == 0u)))
    {
        float4 mesh = cachedMeshBands[index], farValue = cachedFarBands[index];
        float2 meshState = cachedMeshState[index];
        uint meshSource = cachedMeshSources[index];
        float2 farState = cachedFarState[index];
        uint farSource = cachedFarSources[index];
        bool freshMesh = mesh.a < 0, freshFar = farValue.a < 0;
        if (worldAddresses[index] != 0u && (mesh.a < 0 || farValue.a < 0))
        {
            float3 meshSum = 0, farSum = 0;
            [loop] for (uint y = 0; y < span; ++y)
            [loop] for (uint x = 0; x < span; ++x)
            {
                uint member = (local.y + y) * 8u + local.x + x;
                meshSum += sharedMeshContributions[member];
                farSum += sharedFarContributions[member];
            }
            // Mix only a newly traced batch with a geometrically accepted owner.
            // Held/reprojected batches do not advance this accumulation window.
            if (mesh.a < 0 && SSGIWorldMode() != 0u)
            {
                if (SSGIWorldOwnerCacheEnabled() && SSGIWorldOwnerHistoryUpdates() > 1u && oldMeshBatches[index].a >= 0)
                {
                    float weight = rcp(min(meshState.x + 1.0, float(SSGIWorldOwnerHistoryUpdates())));
                    meshSum = lerp(oldMeshBatches[index].rgb, meshSum, weight);
                }
                mesh = float4(meshSum, 0);
            }
#ifdef VXGI_SSGI_GLOBAL
            if (farValue.a < 0 && global.enabled != 0u && SSGIWorldMode() != 0u)
            {
                if (SSGIWorldOwnerCacheEnabled() && SSGIWorldOwnerHistoryUpdates() > 1u && oldFarBatches[index].a >= 0)
                {
                    float weight = rcp(min(farState.x + 1.0, float(SSGIWorldOwnerHistoryUpdates())));
                    farSum = lerp(oldFarBatches[index].rgb, farSum, weight);
                }
                farValue = float4(farSum, 0);
            }
#endif
        }
        RWTexture2D<float4> meshLighting = ResourceDescriptorHeap[sharedMeshLightingTextureIdx];
        RWTexture2D<float4> farLighting = ResourceDescriptorHeap[sharedGlobalLightingTextureIdx];
        uint2 size = (outputSize + 1u) / 2u;
        if (SSGIWorldOwnerCacheEnabled())
        {
            // Advance cold/mature state once per traced batch before the final
            // pixel checker. Replicated coarse slots retain that same state.
            if (worldAddresses[index] != 0u && freshMesh && mesh.a >= 0)
            {
                meshState = SSGIAdvanceWorldOwnerState(meshState);
                meshSource = worldAddresses[index];
            }
            if (worldAddresses[index] != 0u && freshFar && farValue.a >= 0)
            {
                farState = SSGIAdvanceWorldOwnerState(farState);
                farSource = worldAddresses[index];
            }
            RWTexture2D<float2> stateOutput = ResourceDescriptorHeap[currentGlobalOwnerStateIdx];
            [unroll] for (uint q = 0u; q < 4u; ++q)
            {
                if (span == 2u && q != 0u) continue;
                uint2 tile = group.xy * 4u + local / 2u + uint2(q & 1u, q >> 1u);
                if (all(tile < size))
                {
                    stateOutput[tile] = meshState;
                    stateOutput[tile + uint2(size.x, 0u)] = farState;
                }
            }
        }
        [unroll] for (uint q = 0; q < 4; ++q)
        {
            if (span == 2u && q != 0u) continue;
            uint2 tile = group.xy * 4u + local / 2u + uint2(q & 1u, q >> 1u);
            if (all(tile < size))
            {
                meshLighting[tile] = mesh;
                if (SSGIWorldOwnerCacheEnabled())
                {
                    RWTexture2D<uint2> receivers = ResourceDescriptorHeap[sharedGlobalReceiverTextureIdx];
                    receivers[tile] = uint2(worldAddresses[index], meshSource);
                    uint2 farTile = tile + uint2(size.x, 0u);
                    farLighting[farTile] = farValue;
                    receivers[farTile] = uint2(worldAddresses[index], farSource);
                }
                else
                {
                    RWTexture2D<uint> meshReceivers = ResourceDescriptorHeap[sharedMeshReceiverTextureIdx];
                    RWTexture2D<uint> farReceivers = ResourceDescriptorHeap[sharedGlobalReceiverTextureIdx];
                    meshReceivers[tile] = worldAddresses[index];
                    farLighting[tile] = farValue;
                    farReceivers[tile] = worldAddresses[index];
                }
            }
        }
    }
}
#elif defined(SSGI_SHARED_CLEAR_PASS)
[numthreads(1, 1, 1)]
void CSMain_ClearRepairs()
{
    RWTexture2D<uint> count = ResourceDescriptorHeap[sharedRepairCountTextureIdx];
    count[uint2(0, 0)] = 0;
}

#elif defined(SSGI_SHARED_REPAIR_PASS)
#if defined(VXGI_SSGI_THRESHOLD) && defined(VXGI_SSGI_GLOBAL)
void EvaluateReceiverRepairBands(uint2 receiverGI, uint span,
    bool requestScreen, bool requestMesh, bool requestFar,
    out float4 screenValue, out float4 meshValue, out float4 farValue,
    out uint meshKind, out uint farKind)
{
    screenValue = float4(0, 0, 0, -1);
    meshValue = screenValue;
    farValue = screenValue;
    meshKind = SSGI_WORLD_CACHE_INVALID;
    farKind = SSGI_WORLD_CACHE_INVALID;
    // Resolve each cache independently before deciding whether two world
    // estimates can share a freshly traced origin/direction prefix.
    bool freshScreen = false, freshMesh = false, freshFar = false;
    if (requestScreen)
        freshScreen = !ReprojectDistanceBand(receiverGI, previousScreenBandTextureIdx, 2u, screenValue);
    if (requestMesh)
        freshMesh = !ReprojectWorldBand(receiverGI, previousMeshBandIndex, 2u, span, true, meshValue, meshKind);
    if (requestFar)
        freshFar = !ReprojectWorldBand(receiverGI, previousGlobalBandIndex, 4u, span, true, farValue, farKind);
    bool shareWorldPrefix = freshMesh && freshFar;
    if (freshScreen || freshMesh)
    {
        uint2 receiver = receiverGI * SSGITraceFactor(traceDownsampleFactor);
        Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
        Texture2D<float4> normals = ResourceDescriptorHeap[normalTextureIdx];
        Texture2DArray<uint2> blueNoise = ResourceDescriptorHeap[blueNoiseTextureIdx];
        float z = depth.Load(int3(receiver, 0));
        float3 position = ReconstructPosition((receiver + 0.5) / sceneSize, z, invProjMat);
        float3 n = SSGIDecodeViewNormal(normals.Load(int3(receiver, 0)).xyz, viewMat);
        float4 cachedWorldOffset = float4(0, 0, 0, -1);
        float3 screenSum = 0, meshSum = 0, farSum = 0;
        uint worldSamples = SSGIWorldRayCount(span);
        uint sampleCount = max(freshScreen ? 4u : 0u, freshMesh ? worldSamples : 0u);
        [loop] for (uint ray = 0; ray < sampleCount; ++ray)
        {
            float3 screen, mesh, farContribution;
            TraceReceiverDirectionBands(receiverGI, receiver, position, n, blueNoise,
                freshScreen && ray < 4u, freshMesh && ray < worldSamples,
                shareWorldPrefix && ray < worldSamples, ray, cachedWorldOffset,
                screen, mesh, farContribution);
            screenSum += screen;
            meshSum += mesh;
            farSum += farContribution;
        }
        if (freshScreen) screenValue = float4(screenSum * 0.25, 0);
        if (freshMesh)
        {
            meshValue = float4(meshSum / float(worldSamples), 0);
            meshKind = SSGI_WORLD_CACHE_POINT;
        }
        if (shareWorldPrefix)
        {
            farValue = float4(farSum / float(worldSamples), 0);
            farKind = SSGI_WORLD_CACHE_POINT;
        }
    }
    // Screen + Far without fresh Mesh keeps its two original direction
    // sequences: screen index16 and far index32. Only dual-world work couples.
    if (freshFar && !shareWorldPrefix)
    {
        farValue = float4(TraceReceiverBand(receiverGI, 2u, SSGIWorldRayCount(span)).rgb, 0);
        farKind = SSGI_WORLD_CACHE_POINT;
    }
}
#endif

[numthreads(8, 8, 1)]
void CSMain_RepairWorld(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    // One owner per receiver; only failed contributions are repaired.
    if ((lane & 3u) != 0u) return;
    uint start = (group.y * ((outputSize.x + 3u) / 4u) + group.x) * 16u;
    RWTexture2D<uint> count = ResourceDescriptorHeap[sharedRepairCountTextureIdx];
    uint index = start + (lane >> 2u);
    if (index >= count[uint2(0, 0)]) return;
    RWTexture2D<uint> queue = ResourceDescriptorHeap[sharedRepairQueueTextureIdx];
    uint packed = queue[uint2(index % outputSize.x, index / outputSize.x)];
    uint flat = packed >> 3u;
    uint2 receiverGI = uint2(flat % outputSize.x, flat / outputSize.x);
    RWTexture2D<float4> screenCache = ResourceDescriptorHeap[currentScreenBandTextureIdx];
    RWTexture2D<float4> meshCache = ResourceDescriptorHeap[currentMeshBandIndex];
    RWTexture2D<float4> farCache = ResourceDescriptorHeap[currentGlobalBandIndex];
    RWTexture2D<uint> policies = ResourceDescriptorHeap[currentWorldPolicyTextureIdx];
    uint policy = policies[receiverGI];
    uint span = SSGIWorldSpan(policy);
#if defined(VXGI_SSGI_THRESHOLD) && defined(VXGI_SSGI_GLOBAL)
    float4 screenValue, meshValue, farValue;
    uint meshKind, farKind;
    EvaluateReceiverRepairBands(receiverGI, span, (packed & 1u) != 0u,
        (packed & 2u) != 0u, (packed & 4u) != 0u,
        screenValue, meshValue, farValue, meshKind, farKind);
#endif
    float3 addition = 0;
    if ((packed & 3u) != 0u)
    {
#if !defined(VXGI_SSGI_THRESHOLD) || !defined(VXGI_SSGI_GLOBAL)
        float4 screenValue, meshValue;
        uint meshKind;
        EvaluateReceiverNearBands(receiverGI, span, (packed & 1u) != 0u, (packed & 2u) != 0u,
            screenValue, meshValue, meshKind);
#endif
        if ((packed & 1u) != 0u)
        {
            screenCache[receiverGI] = screenValue;
            addition += screenValue.rgb;
        }
        if ((packed & 2u) != 0u)
        {
            meshCache[receiverGI] = meshValue;
            policy = SSGISetBandKind(policy, 1u, meshKind);
            if (SSGIWorldOwnerCacheEnabled())
            {
                RWTexture2D<float4> display = ResourceDescriptorHeap[globalDisplayTextureIdx];
                display[receiverGI] += float4(meshValue.rgb, 0);
            }
            else addition += meshValue.rgb;
        }
    }
    if ((packed & 4u) != 0u)
    {
#if !defined(VXGI_SSGI_THRESHOLD) || !defined(VXGI_SSGI_GLOBAL)
        float4 farValue;
        uint farKind;
        if (!ReprojectWorldBand(receiverGI, previousGlobalBandIndex, 4u, span, true, farValue, farKind))
        {
            farValue = float4(TraceReceiverBand(receiverGI, 2u, SSGIWorldRayCount(span)).rgb, 0);
            farKind = SSGI_WORLD_CACHE_POINT;
        }
#endif
        farCache[receiverGI] = farValue;
        policy = SSGISetBandKind(policy, 2u, farKind);
        if (SSGIWorldOwnerCacheEnabled())
        {
            RWTexture2D<float4> display = ResourceDescriptorHeap[globalDisplayTextureIdx];
            display[receiverGI] += float4(farValue.rgb, 0);
        }
        else addition += farValue.rgb;
    }
    policies[receiverGI] = policy;
    RWTexture2D<float4> output = ResourceDescriptorHeap[outputTextureIdx];
    float4 estimate = output[receiverGI];
    output[receiverGI] = float4(estimate.rgb + addition, estimate.a);
}

#else

bool GatherCoarseSharedWorld(uint2 receiverGI, float3 position, float3 n,
    uint lightingIndex, uint receiverIndex, uint atlasOffsetX, out float4 lighting)
{
    lighting = 0;
    Texture2D<float4> samples = ResourceDescriptorHeap[lightingIndex];
    Texture2D<uint> receivers = ResourceDescriptorHeap[receiverIndex];
    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    Texture2D<float4> normals = ResourceDescriptorHeap[normalTextureIdx];
    uint factor = SSGITraceFactor(traceDownsampleFactor);
    uint2 receiver = receiverGI * factor;
    float z = depth.Load(int3(receiver, 0));
    float footprint = max(SameDepthPixelFootprint((receiver + 0.5) / sceneSize,
        z, position, sceneSize, invProjMat), 1e-4);
    float tolerance = max(1.5 * footprint, 0.002);
    uint2 atlasSize = (outputSize + 1u) / 2u;
    uint2 parentSize = (outputSize + 3u) / 4u;
    // Fine nodes retain their original phase at 2*k. A coarse node replaces
    // four fine nodes centered at 4*k+1, rather than four independent copies.
    int2 base = int2(floor((float2(receiverGI) - 1.0) / 4.0));
    float3 sum = 0;
    float weightSum = 0;
    float maxAge = 0;
    [loop] for (uint i = 0u; i < 4u; ++i)
    {
        int2 parent = base + int2(i & 1u, i >> 1u);
        if (any(parent < 0) || any(parent >= int2(parentSize))) continue;
        float2 parentWeights = saturate(1.0 - abs(float2(receiverGI) - (float2(parent) * 4.0 + 1.0)) / 4.0);
        float parentWeight = parentWeights.x * parentWeights.y;
        if (parentWeight <= 0) continue;
        uint2 parentTile = uint2(parent) * 2u;
        uint canonicalAddress = receivers.Load(int3(parentTile + uint2(atlasOffsetX, 0u), 0));
        bool coarse = canonicalAddress != 0u && SSGIAddressSpan(canonicalAddress) == 4u;
        // A fine parent can have an empty first child. Inspect only children
        // whose native fine tent covers this receiver, without borrowing any
        // replicated coarse slot as an additional sample.
        uint childCount = coarse ? 1u : 4u;
        [loop] for (uint child = 0u; child < childCount; ++child)
        {
            uint2 quadrant = uint2(child & 1u, child >> 1u);
            uint2 tile = parentTile + quadrant;
            if (any(tile >= atlasSize)) continue;
            float spatialWeight = parentWeight;
            uint address = canonicalAddress;
            if (!coarse)
            {
                float2 fineCenter = float2(uint2(parent) * 4u + quadrant * 2u);
                float2 fineWeights = saturate(1.0 - abs(float2(receiverGI) - fineCenter) / 2.0);
                spatialWeight = fineWeights.x * fineWeights.y;
                if (spatialWeight <= 0) continue;
                if (child != 0u) address = receivers.Load(int3(tile + uint2(atlasOffsetX, 0u), 0));
                if (address == 0u || SSGIAddressSpan(address) != 2u) continue;
            }
            uint2 tapGI = SharedReceiverCoordinate(address);
            uint2 tap = tapGI * factor;
            float tapZ = depth.Load(int3(tap, 0));
            float3 q = ReconstructPosition((tap + 0.5) / sceneSize, tapZ, invProjMat);
            float3 tapShading = normalize(DecodeGBufferNormal(normals.Load(int3(tap, 0)).xyz));
            float3 m = normalize(mul(float4(SSGIReceiverGeometricNormal(tap, tapShading), 0), viewMat).xyz);
            float agreement = saturate(dot(n, m));
            float separation = distance(q, position);
            float curvatureAllowance = 0.75 * separation * sqrt(saturate(1.0 - agreement * agreement));
            float planeError = SymmetricPlaneDistance(q - position, n, m) / (tolerance + curvatureAllowance);
            if (agreement < 0.9 || planeError > 1.0) continue;
            float weight = SSGIApplyBilateralGeometryWeight(spatialWeight, agreement, planeError);
            float4 band = samples.Load(int3(tile + uint2(atlasOffsetX, 0u), 0));
            if (!all(isfinite(band)) || band.a < 0 || weight <= 0) continue;
            sum += band.rgb * weight;
            maxAge = max(maxAge, band.a);
            weightSum += weight;
        }
    }
    if (weightSum < 0.01) return false;
    lighting = float4(sum / weightSum, maxAge);
    return true;
}

bool GatherSharedWorld(uint2 receiverGI, float3 position, float3 n, uint span,
    uint lightingIndex, uint receiverIndex, uint requiredSpan, uint atlasOffsetX,
    out float4 lighting)
{
    if (requiredSpan == 4u)
        return GatherCoarseSharedWorld(receiverGI, position, n,
            lightingIndex, receiverIndex, atlasOffsetX, lighting);
    lighting = 0;
    Texture2D<float4> samples = ResourceDescriptorHeap[lightingIndex];
    Texture2D<uint> receivers = ResourceDescriptorHeap[receiverIndex];
    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    Texture2D<float4> normals = ResourceDescriptorHeap[normalTextureIdx];
    uint factor = SSGITraceFactor(traceDownsampleFactor);
    uint2 receiver = receiverGI * factor;
    float z = depth.Load(int3(receiver, 0));
    float footprint = max(SameDepthPixelFootprint((receiver + 0.5) / sceneSize,
        z, position, sceneSize, invProjMat), 1e-4);
    float tolerance = max(1.5 * footprint, 0.002);
    float representativeOrigin = float(sharedMeshSpan / 2u - 1u);
    float2 grid = (float2(receiverGI) - representativeOrigin) / float(span);
    int2 base = int2(floor(grid));
    float2 fraction = frac(grid);
    uint2 size = (outputSize + span - 1u) / span;
    float weightSum = 0;
    float maxAge = 0;
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        int2 offset = int2(i & 1u, i >> 1u);
        int2 tile = base + offset;
        if (any(tile < 0) || any(tile >= int2(size))) continue;
        tile.x += int(atlasOffsetX);
        uint address = receivers.Load(int3(tile, 0));
        if (address == 0) continue;
        // A fine world receiver must not inherit a coarse estimate. Coarse
        // receivers may use compatible fine neighbors; screen has no such tag.
        if (requiredSpan == 2u && SSGIAddressSpan(address) == 4u) continue;
        uint2 tapGI = SharedReceiverCoordinate(address);
        uint2 tap = tapGI * factor;
        float tapZ = depth.Load(int3(tap, 0));
        float3 q = ReconstructPosition((tap + 0.5) / sceneSize, tapZ, invProjMat);
        float3 tapShading = normalize(DecodeGBufferNormal(normals.Load(int3(tap, 0)).xyz));
        float3 m = normalize(mul(float4(SSGIReceiverGeometricNormal(tap, tapShading), 0), viewMat).xyz);
        float agreement = saturate(dot(n, m));
        float separation = distance(q, position);
        // Different tangent planes are expected on one curved surface. Allow
        // their normal rotation, while parallel displaced layers retain the
        // original tight plane tolerance and opposite faces remain rejected.
        float curvatureAllowance = 0.75 * separation * sqrt(saturate(1.0 - agreement * agreement));
        float planeError = SymmetricPlaneDistance(q - position, n, m) / (tolerance + curvatureAllowance);
        // The four screen-neighbor taps already bound the footprint. Euclidean
        // distance rejects valid coplanar taps at grazing view angles.
        if (agreement < 0.9 || planeError > 1.0) continue;
        float2 bilinear = lerp(1.0 - fraction, fraction, float2(offset));
        float weight = SSGIApplyBilateralGeometryWeight(bilinear.x * bilinear.y, agreement, planeError);
        float4 band = samples.Load(int3(tile, 0));
        if (!all(isfinite(band)) || band.a < 0 || weight <= 0) continue;
        lighting.rgb += band.rgb * weight;
        maxAge = max(maxAge, band.a);
        weightSum += weight;
    }
    if (weightSum < 0.01)
    {
        lighting = 0;
        return false;
    }
    lighting = float4(lighting.rgb / weightSum, maxAge);
    return true;
}

bool CarrySharedBand(uint2 receiverGI, uint lightingIndex, uint addressIndex,
    uint requiredSpan, uint atlasOffsetX, out float4 value)
{
    value = float4(0, 0, 0, -1);
    Texture2D<uint> addresses = ResourceDescriptorHeap[addressIndex];
    Texture2D<float4> lighting = ResourceDescriptorHeap[lightingIndex];
    uint2 tile = receiverGI / 2u + uint2(atlasOffsetX, 0u);
    uint address = addresses.Load(int3(tile, 0));
    if (address == 0u || (requiredSpan == 2u && SSGIAddressSpan(address) == 4u)) return false;
    value = lighting.Load(int3(tile, 0));
    if (!all(isfinite(value)) || value.a < 0)
    {
        value = float4(0, 0, 0, -1);
        return false;
    }
    return true;
}

[numthreads(8, 8, 1)]
void CSMain_Trace(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    // Keep the existing checkerboard lane permutation and skip criterion.
    uint compact = lane & 31u;
    uint y = compact >> 2u;
    uint x = (compact & 3u) * 2u + ((y + frameIndex + (lane >> 5u)) & 1u);
    id.xy = group.xy * 8u + uint2(x, y);
    if (any(id.xy >= outputSize)) return;
    RWTexture2D<float4> output = ResourceDescriptorHeap[outputTextureIdx];
    RWTexture2D<float4> reprojectedGI = ResourceDescriptorHeap[reprojectedGITextureIdx];
    RWTexture2D<float4> reprojectedFast = ResourceDescriptorHeap[reprojectedFastTextureIdx];
    RWTexture2D<float4> screenCache = ResourceDescriptorHeap[currentScreenBandTextureIdx];
    RWTexture2D<float4> meshCache = ResourceDescriptorHeap[currentMeshBandIndex];
    RWTexture2D<float4> farCache = ResourceDescriptorHeap[currentGlobalBandIndex];
    RWTexture2D<uint> policies = ResourceDescriptorHeap[currentWorldPolicyTextureIdx];
    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    Texture2D<float4> normals = ResourceDescriptorHeap[normalTextureIdx];
    Texture2D<float4> orm = ResourceDescriptorHeap[ormTextureIdx];
    uint factor = SSGITraceFactor(traceDownsampleFactor);
    uint2 pixel = id.xy * factor;
    float z = depth.Load(int3(pixel, 0));
    output[id.xy] = 0;
    if (SSGIWorldOwnerCacheEnabled())
    {
        RWTexture2D<float4> display = ResourceDescriptorHeap[globalDisplayTextureIdx];
        display[id.xy] = 0;
    }
    reprojectedGI[id.xy] = 0;
    reprojectedFast[id.xy] = 0;
    if (!isfinite(z) || z < 0 || z >= 1 || orm.Load(int3(pixel, 0)).a < 0.5)
    {
        screenCache[id.xy] = float4(0, 0, 0, -1);
        meshCache[id.xy] = float4(0, 0, 0, -1);
        farCache[id.xy] = float4(0, 0, 0, -1);
        policies[id.xy] = 0u;
        return;
    }
#ifdef SSGI_SHARED_TRACE
    uint policy = policies[id.xy];
#else
    uint policy = SSGIPolicyForSpan(SSGIWorldMode() != 0u ? SSGICameraSpan(id.xy) : 2u);
#endif
    uint span = SSGIWorldSpan(policy);
    if (historyValid)
    {
        float4 history, fastHistory;
        float3 worldNormal = normalize(DecodeGBufferNormal(normals.Load(int3(pixel, 0)).xyz));
        if (SSGIReprojectHistory(pixel, sceneSize, outputSize, factor, z, worldNormal, invViewProjMat,
            prevJitteredViewProj, prevJitteredInvViewProj, previousGITextureIdx, previousDepthTextureIdx,
            previousNormalTextureIdx, previousFastTextureIdx, history, fastHistory))
        {
            reprojectedGI[id.xy] = history;
            reprojectedFast[id.xy] = fastHistory;
            if (((id.x + id.y + frameIndex) & 1u) != 0u)
            {
                // Band due dates and camera quality never turn this phase into
                // tracing. Carry each independent estimate with its actual age.
                float4 screenValue = float4(0, 0, 0, -1), meshValue = screenValue, farValue = screenValue;
                uint meshKind = 0u, farKind = 0u;
#ifdef SSGI_SHARED_TRACE
                CarrySharedBand(id.xy, sharedScreenLightingTextureIdx, sharedScreenReceiverTextureIdx, 0u, 0u, screenValue);
                if (SSGIWorldMode() != 0u)
                {
                    if (CarrySharedBand(id.xy, sharedMeshLightingTextureIdx, sharedMeshReceiverTextureIdx, span, 0u, meshValue))
                        meshKind = SSGIWorldKind(span);
#ifdef VXGI_SSGI_GLOBAL
                    if (global.enabled != 0u && CarrySharedBand(id.xy,
                        sharedGlobalLightingTextureIdx, sharedGlobalReceiverTextureIdx, span,
                        SSGIWorldOwnerCacheEnabled() ? (outputSize.x + 1u) / 2u : 0u, farValue))
                        farKind = SSGIWorldKind(span);
#endif
                }
#else
                ReprojectDistanceBand(id.xy, previousScreenBandTextureIdx, 0u, screenValue);
                if (SSGIWorldMode() != 0u)
                {
                    ReprojectWorldBand(id.xy, previousMeshBandIndex, 0u, span, true, meshValue, meshKind);
#ifdef VXGI_SSGI_GLOBAL
                    if (global.enabled != 0u)
                        ReprojectWorldBand(id.xy, previousGlobalBandIndex, 0u, span, true, farValue, farKind);
#endif
                }
#endif
                screenCache[id.xy] = screenValue;
                meshCache[id.xy] = meshValue;
                farCache[id.xy] = farValue;
                policy = SSGISetBandKind(SSGISetBandKind(policy, 1u, meshKind), 2u, farKind);
                policies[id.xy] = policy;
                output[id.xy] = float4(history.rgb, -1);
                return;
            }
        }
    }
    float4 screenValue = float4(0, 0, 0, -1), meshValue = screenValue, farValue = screenValue;
    uint meshKind = 0u, farKind = 0u;
#ifdef SSGI_SHARED_TRACE
    float3 position = ReconstructPosition((pixel + 0.5) / sceneSize, z, invProjMat);
    float3 shadingWorld = normalize(DecodeGBufferNormal(normals.Load(int3(pixel, 0)).xyz));
    float3 guideNormal = normalize(mul(float4(SSGIReceiverGeometricNormal(pixel, shadingWorld), 0), viewMat).xyz);
    bool repairScreen = !GatherSharedWorld(id.xy, position, guideNormal, 2u,
        sharedScreenLightingTextureIdx, sharedScreenReceiverTextureIdx, 0u, 0u, screenValue);
    bool repairMesh = false, repairFar = false;
    if (SSGIWorldMode() != 0u)
    {
        repairMesh = !GatherSharedWorld(id.xy, position, guideNormal, 2u,
            sharedMeshLightingTextureIdx, sharedMeshReceiverTextureIdx, span, 0u, meshValue);
        if (!repairMesh) meshKind = SSGIWorldKind(span);
#ifdef VXGI_SSGI_GLOBAL
        if (global.enabled != 0u)
        {
            repairFar = !GatherSharedWorld(id.xy, position, guideNormal, 2u,
                sharedGlobalLightingTextureIdx, sharedGlobalReceiverTextureIdx, span,
                SSGIWorldOwnerCacheEnabled() ? (outputSize.x + 1u) / 2u : 0u, farValue);
            if (!repairFar) farKind = SSGIWorldKind(span);
        }
#endif
    }
    if (repairScreen || repairMesh || repairFar)
    {
        RWTexture2D<uint> queue = ResourceDescriptorHeap[sharedRepairQueueTextureIdx];
        RWTexture2D<uint> count = ResourceDescriptorHeap[sharedRepairCountTextureIdx];
        uint first = 0;
        uint waveCount = WaveActiveCountBits(true);
        if (WaveIsFirstLane()) InterlockedAdd(count[uint2(0, 0)], waveCount, first);
        first = WaveReadLaneFirst(first);
        uint index = first + WavePrefixCountBits(true);
        uint flags = (repairScreen ? 1u : 0u) | (repairMesh ? 2u : 0u) | (repairFar ? 4u : 0u);
        queue[uint2(index % outputSize.x, index / outputSize.x)] =
            ((id.y * outputSize.x + id.x) << 3u) | flags;
    }
#else
    EvaluateReceiverNearBands(id.xy, span, true, SSGIWorldMode() != 0u,
        screenValue, meshValue, meshKind);
    if (SSGIWorldMode() != 0u)
    {
#ifdef VXGI_SSGI_GLOBAL
        if (global.enabled != 0u &&
            !ReprojectWorldBand(id.xy, previousGlobalBandIndex, 4u, span, true, farValue, farKind))
        {
            farValue = float4(TraceReceiverBand(id.xy, 2u, SSGIWorldRayCount(span)).rgb, 0);
            farKind = SSGI_WORLD_CACHE_POINT;
        }
#endif
    }
#endif
    screenCache[id.xy] = screenValue;
    meshCache[id.xy] = meshValue;
    farCache[id.xy] = farValue;
    policies[id.xy] = SSGISetBandKind(SSGISetBandKind(policy, 1u, meshKind), 2u, farKind);
#ifdef SSGI_SHARED_TRACE
    if (SSGIWorldOwnerCacheEnabled())
    {
        RWTexture2D<float4> display = ResourceDescriptorHeap[globalDisplayTextureIdx];
        display[id.xy] = float4(meshValue.rgb + farValue.rgb, 1);
        // Active raw pixels contain the signal needing pixel reconstruction.
        // World bands are already gathered and added once by that pass.
        output[id.xy] = float4(screenValue.rgb, 1);
    }
    else
#endif
    output[id.xy] = float4(screenValue.rgb + meshValue.rgb + farValue.rgb, 1);
}

#endif // SSGI_SHARED_WORLD_PASS

#elif defined(SSGI_PASS_COMPOSITE)

#include "SSGICommon.hlsli"
#include "MaterialCommon.hlsli"
#include "FullscreenTriangle.hlsli"

cbuffer SSGICompositeConstants : register(b0)
{
    float4x4 viewMat;
    float4x4 invProjMat;
    uint2 sceneSize;
    uint2 halfSize;
    uint depthTextureIdx;
    uint normalTextureIdx;
    uint ormTextureIdx;
    uint albedoTextureIdx;
    uint giTextureIdx;
    float intensity;
    uint traceDownsampleFactor;
    uint padding;
};

float4 VSMain(uint id : SV_VertexID) : SV_POSITION
{
    return GetFullscreenTrianglePosition(GetFullscreenTriangleTexCoord(id));
}

float4 PSMain(float4 position : SV_POSITION) : SV_Target0
{
    uint2 pixel = uint2(position.xy);
    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    Texture2D<float4> normal = ResourceDescriptorHeap[normalTextureIdx];
    Texture2D<float4> orm = ResourceDescriptorHeap[ormTextureIdx];
    Texture2D<float4> albedoTexture = ResourceDescriptorHeap[albedoTextureIdx];
    Texture2D<float4> gi = ResourceDescriptorHeap[giTextureIdx];
    float z = depth.Load(int3(pixel, 0));
    float4 material = orm.Load(int3(pixel, 0));
    if (z >= 1 || material.a < 0.5)
    {
        return 0;
    }

    float2 uv = (pixel + 0.5) / sceneSize;
    float3 p = ReconstructPosition(uv, z, invProjMat);
    float3 n = SSGIDecodeViewNormal(normal.Load(int3(pixel, 0)).xyz, viewMat);
    float footprint = SameDepthPixelFootprint(uv, z, p, sceneSize, invProjMat);
    float tolerance = max(min(footprint * 0.5, p.z * 0.01), 0.001);
    uint factor = SSGITraceFactor(traceDownsampleFactor);
    float inverseFactor = factor == 2u ? 0.5 : rcp(float(factor));
    // Signal texel i samples full-resolution pixel factor*i, including odd extents.
    float2 halfPixel = padding != 0u ? (float2(pixel) + 0.5) * inverseFactor - 0.5 : float2(pixel) * inverseFactor;
    int2 base = padding != 0u ? int2(floor(halfPixel + 0.5)) - 1 : int2(floor(halfPixel));
    float2 fraction = frac(halfPixel);
    bool fullResolution = factor == 1u && padding == 0u;
    float3 sum = fullResolution ? gi.Load(int3(pixel, 0)).rgb : 0;
    float weightSum = fullResolution ? 1.0 : 0.0;

    [loop]
    for (int i = 0; i < (fullResolution ? 0 : (padding != 0u ? 9 : 4)); ++i)
    {
        int2 offset = padding != 0u ? int2(i % 3, i / 3) : int2(i & 1, i >> 1);
        int2 tap = base + offset;
        if (any(tap < 0) || any(tap >= int2(halfSize)))
        {
            continue;
        }
        uint2 fullPixel = SSGIReceiverPixel(uint2(tap), factor, sceneSize, padding);
        if (any(fullPixel >= sceneSize)) continue;
        float tapZ = depth.Load(int3(fullPixel, 0));
        if (tapZ >= 1 || orm.Load(int3(fullPixel, 0)).a < 0.5)
        {
            continue;
        }

        float3 tapN = SSGIDecodeViewNormal(normal.Load(int3(fullPixel, 0)).xyz, viewMat);
        float agreement = saturate(dot(n, tapN));
        if (agreement < 0.8)
        {
            continue;
        }
        float3 q = ReconstructPosition((fullPixel + 0.5) / sceneSize, tapZ, invProjMat);
        float3 delta = q - p;
        float planeError = SymmetricPlaneDistance(delta, n, tapN) / tolerance;

        // Preserve coplanar surfaces using plane distance rather than point distance.
        if (planeError >= 3)
        {
            continue;
        }
        float2 bilinear = lerp(1 - fraction, fraction, float2(offset));
        float2 separation = float2(tap) - halfPixel;
        float spatialWeight = padding != 0u ? exp2(-2.0 * dot(separation, separation)) : bilinear.x * bilinear.y;
        float weight = SSGIApplyBilateralGeometryWeight(spatialWeight, agreement, planeError);

        sum += gi.Load(int3(tap, 0)).rgb * weight; // Spatial variance in alpha is never a lighting factor.
        weightSum += weight;
    }
    float3 indirect = weightSum > 1e-5 ? sum / weightSum : 0;

#ifdef SSGI_DISPLAY_IRRADIANCE
    // Display the completed GI signal (E/pi), without the receiver's material response.
    return float4(indirect * intensity, 0);
#else
    float3 albedo = albedoTexture.Load(int3(pixel, 0)).rgb;
    float3 F = fresnelSchlickRoughness(saturate(dot(n, normalize(-p))), ComputeMaterialF0(albedo, material.b),
                                       ClampPerceptualRoughness(material.g));
    // Input already stores E/pi. Apply receiver diffuse response once; additive blend preserves SceneColor alpha.
    float3 result = indirect * albedo * ComputeDiffuseEnergy(F, material.b) * intensity;

    return float4(result, 0);
#endif
}

#endif
