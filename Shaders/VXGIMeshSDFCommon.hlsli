#ifndef VXGI_MESH_SDF_COMMON_HLSLI
#define VXGI_MESH_SDF_COMMON_HLSLI

#include "VXGIMeshSDFData.hlsli"

VXGISDFGrid VXGIMeshSDFGrid(VXGIMeshSDFAsset asset)
{
    VXGISDFGrid grid;
    grid.origin = asset.origin;
    grid.cellSize = asset.cellSize;
    grid.cellDimensions = asset.cellDimensions;
    grid.boundsMaximum = asset.boundsMaximum;
    return grid;
}

float VXGIMeshSDFConservativeDistance(float3 position, VXGIMeshSDFAsset asset,
    StructuredBuffer<float> nodes)
{
    uint3 node = uint3(clamp(floor((position - asset.origin) / asset.cellSize + 0.5),
        0.0, float3(asset.cellDimensions)));
    float distanceAtNode = nodes[asset.nodeOffset + VXGISDFFlatIndex(node, asset.cellDimensions + 1)];
    float offset = length(position - (asset.origin + float3(node) * asset.cellSize));
    return max(0.0, distanceAtNode - offset - max(asset.cellSize * 1e-4, abs(distanceAtNode) * 2e-6));
}

// World-space diagnostic slice only. This is never used to accept ray hits.
float VXGIMeshSDFDisplayDistance(float3 worldPosition, StructuredBuffer<float> nodes)
{
    // Root SRVs expose an address, not a NumElements descriptor. The immutable
    // record layout repeats this scene count; empty scenes are not dispatched.
    uint instanceCount = VXGIMeshSDFInstances[0].sceneInstanceCount;
    float best = 1e30;
    [loop] for (uint instanceIndex = 0; instanceIndex < instanceCount; ++instanceIndex)
    {
        VXGIMeshSDFInstance instance = VXGIMeshSDFInstances[instanceIndex];
        VXGIMeshSDFAsset asset = VXGIMeshSDFAssets[instance.assetIndex];
        float3 position = mul(float4(worldPosition, 1), instance.localFromWorld).xyz;
        float3 coordinate = clamp((position - asset.origin) / asset.cellSize, 0.0, float3(asset.cellDimensions));
        uint3 cell = min(uint3(floor(coordinate)), asset.cellDimensions - 1);
        float3 f = coordinate - float3(cell);
        uint3 dims = asset.cellDimensions + 1;
        uint first = asset.nodeOffset;
        float d000 = nodes[first + VXGISDFFlatIndex(cell, dims)];
        float d100 = nodes[first + VXGISDFFlatIndex(cell + uint3(1, 0, 0), dims)];
        float d010 = nodes[first + VXGISDFFlatIndex(cell + uint3(0, 1, 0), dims)];
        float d110 = nodes[first + VXGISDFFlatIndex(cell + uint3(1, 1, 0), dims)];
        float d001 = nodes[first + VXGISDFFlatIndex(cell + uint3(0, 0, 1), dims)];
        float d101 = nodes[first + VXGISDFFlatIndex(cell + uint3(1, 0, 1), dims)];
        float d011 = nodes[first + VXGISDFFlatIndex(cell + uint3(0, 1, 1), dims)];
        float d111 = nodes[first + VXGISDFFlatIndex(cell + uint3(1, 1, 1), dims)];
        float localDistance = lerp(lerp(lerp(d000, d100, f.x), lerp(d010, d110, f.x), f.y),
            lerp(lerp(d001, d101, f.x), lerp(d011, d111, f.x), f.y), f.z);
        localDistance += length(position - (asset.origin + coordinate * asset.cellSize));
        float minimumScale = min(length(instance.worldFromLocal[0].xyz),
            min(length(instance.worldFromLocal[1].xyz), length(instance.worldFromLocal[2].xyz)));
        best = min(best, localDistance * minimumScale);
    }
    return best;
}

double VXGIMeshSDFDot(double3 a, double3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

double3 VXGIMeshSDFCross(double3 a, double3 b)
{
    return double3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

// Keep the original world interval and a precise parameter until all instances
// have competed. Float local T cannot distinguish nearby far-away surfaces or
// safely decide whether a transformed hit lies on a finite endpoint.
bool VXGIMeshSDFWorldBounds(float3 origin, float3 direction, float3 minimum,
    float3 maximum, double rayMin, double rayMax)
{
    double entry = rayMin, exit = rayMax;
    [unroll] for (uint axis = 0; axis < 3; ++axis)
    {
        if (direction[axis] == 0.0)
        {
            if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis]) return false;
        }
        else
        {
            double a = (double(minimum[axis]) - double(origin[axis])) / double(direction[axis]);
            double b = (double(maximum[axis]) - double(origin[axis])) / double(direction[axis]);
            entry = max(entry, min(a, b));
            exit = min(exit, max(a, b));
        }
    }
    return entry <= exit;
}

bool VXGIMeshSDFWorldTriangle(float3 rayOrigin, float3 rayDirection,
    VXGISDFTriangle surfaceTriangle, VXGIMeshSDFInstance instance,
    double rayMin, double rayMax, out double distance, out float3 barycentric,
    out float3 geometricNormal)
{
    distance = 0.0;
    barycentric = 0.0;
    geometricNormal = 0.0;
    precise float3 p0 = mul(float4(surfaceTriangle.p0, 1), instance.worldFromLocal).xyz;
    precise float3 p1 = mul(float4(surfaceTriangle.p1, 1), instance.worldFromLocal).xyz;
    precise float3 p2 = mul(float4(surfaceTriangle.p2, 1), instance.worldFromLocal).xyz;
    double3 edge1 = double3(p1) - double3(p0), edge2 = double3(p2) - double3(p0);
    double3 p = VXGIMeshSDFCross(double3(rayDirection), edge2);
    double determinant = VXGIMeshSDFDot(edge1, p);
    float3 normal = cross(p1 - p0, p2 - p0);
    if (abs(determinant) <= 1e-12 * double(length(normal))) return false;
    double inverse = 1.0 / determinant;
    double3 offset = double3(rayOrigin) - double3(p0);
    double u = VXGIMeshSDFDot(offset, p) * inverse;
    double3 q = VXGIMeshSDFCross(offset, edge1);
    double v = VXGIMeshSDFDot(double3(rayDirection), q) * inverse;
    if (u < -2e-5 || v < -2e-5 || u + v > 1.00002) return false;
    distance = VXGIMeshSDFDot(edge2, q) * inverse;
    if (distance < rayMin || distance > rayMax) return false;
    barycentric = float3(1.0 - u - v, u, v);
    geometricNormal = normalize(normal);
    return true;
}

#endif
