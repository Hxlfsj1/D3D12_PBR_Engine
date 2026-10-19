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
};

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
    // Investigation and limitations: VXGI.md, including the PIX diagnostic record.
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

// Transform the original biased SSGI origin and finite direction vector. The
// normalized world ray and its length describe the same two endpoints; no
// receiver re-anchoring, added offset or scene-sized range is introduced.
bool SSGIWorldRaySegment(float3 viewOrigin, float3 viewDirection,
    out float3 worldOrigin, out float3 unitWorldDirection, out float worldRayLength)
{
    worldOrigin = 0.0;
    unitWorldDirection = 0.0;
    worldRayLength = 0.0;
    if (!all(isfinite(viewOrigin)) || !all(isfinite(viewDirection))) return false;
    float viewRayLength = SSGIViewRayLength(viewOrigin, viewDirection);
    float3 transformedDirection = mul(float4(viewDirection, 0.0), invViewMat).xyz;
    worldOrigin = mul(float4(viewOrigin, 1.0), invViewMat).xyz;
    if (!all(isfinite(worldOrigin)) || !SSGINormalizeDirection(transformedDirection, unitWorldDirection)
        || !isfinite(viewRayLength) || viewRayLength <= 0.0) return false;
    worldRayLength = viewRayLength * length(transformedDirection);
    return isfinite(worldRayLength) && worldRayLength > 0.0;
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

// Legacy screen-only path retains the same biased origin, hit acceptance and
// fine-pixel source fetch. Black/unlit source radiance still terminates a hit.
float4 TraceRay(uint2 receiver, float3 origin, float3 direction)
{
    SSGIScreenRayResult screen = TraceScreenRay(receiver, origin, direction);
    if (screen.status != SSGI_SCREEN_HIT) return 0.0;
    Texture2D<float4> source = ResourceDescriptorHeap[sourceTextureIdx];
    return float4(source.Load(int3(screen.hitPixel, 0)).rgb, 1.0);
}

// This per-ray helper is also called directly by headless validation. A valid
// screen hit ALWAYS returns the original SSGI source, including black hits.
SSGIHybridRayResult TraceHybridRay(uint2 receiver, float3 viewOrigin, float3 viewDirection)
{
    SSGIHybridRayResult result = (SSGIHybridRayResult)0;
    result.triangleIndex = VXGI_PROBE_MISSING_INDEX;
    result.worldTraceStatus = 0xffffffffu;
    result.status = SSGI_SCREEN_INVALID;
    SSGIScreenRayResult screen = TraceScreenRay(receiver, viewOrigin, viewDirection);
#ifdef SSGI_RAY_ROUTING_DIAGNOSTICS
    SSGIRoutingScreenStatus(screen.status);
#endif
    result.status = screen.status;
    result.distance = screen.distance;
    result.confidence = screen.confidence;
    if (screen.status == SSGI_SCREEN_HIT)
    {
        Texture2D<float4> source = ResourceDescriptorHeap[sourceTextureIdx];
        result.hit = 1;
        result.backend = SSGI_BACKEND_SCREEN;
        result.radiance = source.Load(int3(screen.hitPixel, 0)).rgb;
        result.sourceValid = all(isfinite(result.radiance)) ? 1u : 0u;
        if (result.sourceValid == 0) result.radiance = 0.0;
        return result;
    }
    if (SSGIWorldMode() == 0)
    {
#ifdef SSGI_RAY_ROUTING_DIAGNOSTICS
        SSGIRoutingDisabled();
#endif
        return result;
    }
#ifdef SSGI_RAY_ROUTING_DIAGNOSTICS
    SSGIRoutingFallback();
#endif
    float3 worldOrigin;
    float3 unitWorldDirection;
    float worldRayLength;
    if (!SSGIWorldRaySegment(viewOrigin, viewDirection, worldOrigin, unitWorldDirection, worldRayLength))
    {
#ifdef SSGI_RAY_ROUTING_DIAGNOSTICS
        SSGIRoutingInvalidSegment();
#endif
        result.status = SSGI_SCREEN_INVALID;
        result.worldTraceStatus = 2;
        return result;
    }
    return SSGIWorldRaySource(worldOrigin, unitWorldDirection, worldRayLength, hybrid, hybridNodes,
#ifndef VXGI_SSGI_THRESHOLD
        hybridOffsets, hybridReferences, hybridTriangles, hybridMaterials,
        hybridLookup, hybridRawRecords, hybridMaterialSampler);
#else
        hybridLookup, hybridRawRecords);
#endif
}

[numthreads(8, 8, 1)]
void CSMain_Trace(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= outputSize))
    {
        return;
    }
    RWTexture2D<float4> output = ResourceDescriptorHeap[outputTextureIdx];
    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    Texture2D<float4> normals = ResourceDescriptorHeap[normalTextureIdx];
    Texture2D<float4> orm = ResourceDescriptorHeap[ormTextureIdx];
    uint factor = max(traceDownsampleFactor, 2u);
    uint2 pixel = id.xy * factor;
    float z = depth.Load(int3(pixel, 0));
    output[id.xy] = 0;
    if (z >= 1 || orm.Load(int3(pixel, 0)).a < 0.5)
    {
        return;
    }

    float3 position = ReconstructPosition((pixel + 0.5) / sceneSize, z, invProjMat);
    float3 n = SSGIDecodeViewNormal(normals.Load(int3(pixel, 0)).xyz, viewMat);
    Texture2DArray<uint2> blueNoise = ResourceDescriptorHeap[blueNoiseTextureIdx];
    float4 sum = 0;
    [loop]
    for (uint ray = 0; ray < rayCount; ++ray)
    {
        float2 xi = LoadSTBN2D(blueNoise, id.xy, frameIndex, ray);
        float3 viewDirection = CosineDirection(xi, n);
        if (SSGIWorldMode() == 0)
        {
            sum += TraceRay(pixel, position + n * normalBias, viewDirection);
            continue;
        }
        SSGIHybridRayResult sample = TraceHybridRay(pixel, position + n * normalBias, viewDirection);
        sum += float4(sample.radiance, float(sample.hit));
    }
    // Cosine-weighted sampling: RGB is E/pi. Normalize by ALL rays, including misses.
    // Alpha stores hit fraction, never an AO factor or normalization weight.
    output[id.xy] = sum / float(rayCount);
}

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
    uint factor = max(traceDownsampleFactor, 2u);
    float inverseFactor = factor == 2u ? 0.5 : rcp(float(factor));
    // Signal texel i samples full-resolution pixel factor*i, including odd extents.
    float2 halfPixel = float2(pixel) * inverseFactor;
    int2 base = int2(floor(halfPixel));
    float2 fraction = frac(halfPixel);
    float3 sum = 0;
    float weightSum = 0;

    [loop]
    for (int i = 0; i < 4; ++i)
    {
        int2 offset = int2(i & 1, i >> 1);
        int2 tap = base + offset;
        if (any(tap >= int2(halfSize)))
        {
            continue;
        }
        int2 fullPixel = tap * int(factor);
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
        float weight = SSGIApplyBilateralGeometryWeight(bilinear.x * bilinear.y, agreement, planeError);

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
