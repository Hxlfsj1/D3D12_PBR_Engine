#include "SSGICommon.hlsli"
#include "BlueNoise.hlsli"

cbuffer Constants : register(b0)
{
    float4x4 View, Proj, InvProj;
    uint2 Size, OutputSize;
    uint DepthIndex, NormalIndex, ORMIndex, SourceIndex;
    uint OutputIndex, LevelCount, FrameIndex, BlueNoiseIndex;
    float Radius, Thickness, NormalBias, Padding2;
    uint RayCount, MaxIterations;
    uint2 Padding3;
    uint4 Levels[7]; // Keep in sync with SSGI::MaxLevels.
};

float3 Project(float3 p)
{
    float4 clip = mul(float4(p, 1), Proj);
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
    float4 clip = mul(float4(0, 0, viewZ, 1), Proj);
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
    float farDepth = DeviceDepth(bounds.y + Thickness);
    entryT = t;
    if (abs(deltaZ) < 1e-8)
        return startZ >= nearDepth && startZ <= farDepth;
    float a = (nearDepth - startZ) / deltaZ;
    float b = (farDepth - startZ) / deltaZ;
    entryT = max(t, min(a, b));
    return entryT <= min(exitT, max(a, b));
}

// Stackless hierarchical grid traversal, ordinary Z. Mip 0 is the full-resolution depth;
// mip 1..LevelCount map to Levels[0..LevelCount-1] (2x2 through 128x128 pixel cells).
float4 TraceRay(uint2 receiver, float3 origin, float3 direction)
{
    Texture2D<float> depth = ResourceDescriptorHeap[DepthIndex];
    Texture2D<float4> normals = ResourceDescriptorHeap[NormalIndex];
    Texture2D<float4> source = ResourceDescriptorHeap[SourceIndex];
    float rayLength = Radius;
    float nearZ = SSGIViewPosition(0.5.xx, 0, InvProj).z;
    if (direction.z < 0) rayLength = min(rayLength, max(0, (origin.z - nearZ * 1.01) / -direction.z));
    float3 start = Project(origin);
    float3 delta = Project(origin + direction * rayLength) - start;
    float endT = 1;
    [unroll] for (uint axis = 0; axis < 2; ++axis)
    {
        if (delta[axis] > 0) endT = min(endT, (1 - start[axis]) / delta[axis]);
        if (delta[axis] < 0) endT = min(endT, -start[axis] / delta[axis]);
    }
    if (any(start.xy < 0) || any(start.xy >= 1) || endT <= 0) return 0;

    float2 startPixels = start.xy * Size;
    float2 deltaPixels = delta.xy * Size;
    // As in UE's Lumen traversal, leave the starting fine cell without testing itself.
    float t = CellExit(startPixels, deltaPixels, RayCell(startPixels, deltaPixels, 0), 0, endT);
    uint mip = 0;
    uint iteration = 0;
    [loop] for (; iteration < MaxIterations && t < endT; ++iteration)
    {
        int2 cell = RayCell(startPixels + deltaPixels * t, deltaPixels, mip);
        uint cellSize = 1u << mip;
        int2 gridSize = int2((Size + cellSize - 1) / cellSize);
        if (any(cell < 0) || any(cell >= gridSize)) break;
        float exitT = CellExit(startPixels, deltaPixels, cell, mip, endT);
        float2 bounds;
        if (mip == 0)
        {
            float z = depth.Load(int3(cell, 0));
            float viewZ = SSGIViewPosition((cell + 0.5) / Size, z, InvProj).z;
            bounds = z < 1 ? viewZ.xx : float2(1e20, 0);
        }
        else
        {
            // Each lane can be at a different mip after adaptive traversal.
            Texture2D<float2> hierarchy = ResourceDescriptorHeap[NonUniformResourceIndex(Levels[mip - 1].x)];
            // Physical cell size, not UV * ceil(mipSize), keeps odd image edges aligned.
            bounds = hierarchy.Load(int3(cell, 0));
        }

        float entryT = t;
        bool potentialHit = false;
        if (bounds.x <= bounds.y)
            potentialHit = DepthInterval(start.z, delta.z, bounds, t, exitT, entryT);
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
            float3 hit = SSGIViewPosition(hitUV, DeviceDepth(bounds.x), InvProj);
            float3 hitNormal = normalize(mul(float4(normals.Load(int3(cell, 0)).xyz * 2 - 1, 0), View).xyz);
            if (distance(hit, origin) <= Radius && dot(hitNormal, direction) < 0)
            {
                // Traversal mip controls geometry only. Fetch color at the confirmed fine hit.
                float3 radiance = source.Load(int3(cell, 0)).rgb;
                return float4(radiance, 1); // Black geometry still terminates the ray.
            }
        }

        // The current cell has been ruled out. Cross its boundary and try a coarser cell.
        t = exitT;
        mip = min(mip + 1, LevelCount);
    }
    return 0; // Screen/radius/iteration limit: no sky fallback and no GI feedback.
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= OutputSize)) return;
    RWTexture2D<float4> output = ResourceDescriptorHeap[OutputIndex];
    Texture2D<float> depth = ResourceDescriptorHeap[DepthIndex];
    Texture2D<float4> normals = ResourceDescriptorHeap[NormalIndex];
    Texture2D<float4> orm = ResourceDescriptorHeap[ORMIndex];
    uint2 pixel = id.xy * 2;
    float z = depth.Load(int3(pixel, 0));
    output[id.xy] = 0;
    if (z >= 1 || orm.Load(int3(pixel, 0)).a < 0.5) return;

    float3 position = SSGIViewPosition((pixel + 0.5) / Size, z, InvProj);
    float3 n = normalize(mul(float4(normals.Load(int3(pixel, 0)).xyz * 2 - 1, 0), View).xyz);
    Texture2DArray<uint2> blueNoise = ResourceDescriptorHeap[BlueNoiseIndex];
    float4 sum = 0;
    [loop] for (uint ray = 0; ray < RayCount; ++ray)
    {
        float2 xi = LoadSTBN2D(blueNoise, id.xy, FrameIndex, ray);
        sum += TraceRay(pixel, position + n * NormalBias, CosineDirection(xi, n));
    }
    // Cosine-weighted sampling: RGB is E/pi. Normalize by ALL rays, including misses.
    // Alpha stores hit fraction, never an AO factor or normalization weight.
    output[id.xy] = sum / float(RayCount);

}
