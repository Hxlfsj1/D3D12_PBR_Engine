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
    uint2 padding3;
    uint4 levels[7]; // Keep in sync with SSGIMaxLevels.
};

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

// Stackless hierarchical grid traversal, ordinary Z. Mip 0 is the full-resolution depth;
// mip 1..LevelCount map to Levels[0..LevelCount-1] (2x2 through 128x128 pixel cells).
float4 TraceRay(uint2 receiver, float3 origin, float3 direction)
{
    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    Texture2D<float4> normals = ResourceDescriptorHeap[normalTextureIdx];
    Texture2D<float4> source = ResourceDescriptorHeap[sourceTextureIdx];
    float rayLength = radius;
    float nearZ = ReconstructPosition(0.5.xx, 0, invProjMat).z;
    if (direction.z < 0)
    {
        rayLength = min(rayLength, max(0, (origin.z - nearZ * 1.01) / -direction.z));
    }
    float3 start = Project(origin);
    float3 delta = Project(origin + direction * rayLength) - start;
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
        return 0;
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
            break;
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
                // Traversal mip controls geometry only. Fetch color at the confirmed fine hit.
                float3 radiance = source.Load(int3(cell, 0)).rgb;
                return float4(radiance, 1); // Black geometry still terminates the ray.
            }
        }

        // The current cell has been ruled out. Cross its boundary and try a coarser cell.
        t = exitT;
        mip = min(mip + 1, levelCount);
    }
    return 0; // Screen/radius/iteration limit: no sky fallback and no GI feedback.
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
    uint2 pixel = id.xy * 2;
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
        sum += TraceRay(pixel, position + n * normalBias, CosineDirection(xi, n));
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
    uint2 padding;
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
    float footprint = SSGISameDepthPixelFootprint(uv, z, p, sceneSize, invProjMat);
    float tolerance = max(min(footprint * 0.5, p.z * 0.01), 0.001);
    float2 halfPixel = float2(pixel) * 0.5;
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
        int2 fullPixel = tap * 2;
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
        float planeError = SSGISymmetricPlaneDistance(delta, n, tapN) / tolerance;

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

    float3 albedo = albedoTexture.Load(int3(pixel, 0)).rgb;
    float3 F = fresnelSchlickRoughness(saturate(dot(n, normalize(-p))), ComputeMaterialF0(albedo, material.b),
                                       ClampPerceptualRoughness(material.g));
    // Input already stores E/pi. Apply receiver diffuse response once; additive blend preserves SceneColor alpha.
    float3 result = indirect * albedo * ComputeDiffuseEnergy(F, material.b) * intensity;

    return float4(result, 0);
}

#endif
