#ifndef VXGI_SDF_COMMON_HLSLI
#define VXGI_SDF_COMMON_HLSLI

#include "TangentBasis.hlsli"
#include "VXGIMaterialCommon.hlsli"

// Scalar-packed StructuredBuffer layout, matching VXGISDFDebugRenderer::GpuTriangle.
// Winding/geometricNormal retain the real surface orientation; tracing is double-sided.
struct VXGISDFTriangle
{
    float3 p0; uint triangleId;
    float3 p1; uint flags;
    float3 p2; uint pad0;
    float3 geometricNormal; uint pad1;
    float2 uv0; float2 uv1; float2 uv2;
    float3 shadingNormal0; float3 shadingNormal1; float3 shadingNormal2;
    float3 tangent0; float3 tangent1; float3 tangent2;
    float3 bitangent0; float3 bitangent1; float3 bitangent2;
    uint materialId; uint instanceId; uint pad2;
};

struct VXGISDFGrid
{
    float3 origin;
    float cellSize;
    uint3 cellDimensions;
    float3 boundsMaximum;
};

struct VXGISDFHit
{
    float3 position;
    float distance;
    float3 geometricNormal;
    uint triangleId;
    float3 barycentric;
    uint triangleIndex;
    uint instanceIndex; // Mesh backend instance; legacy triangles already carry world metadata.
    uint backFace;
    uint valid;
    uint traceStatus; // 0: verified miss, 1: hit, 2: invalid ray/coverage, 3: traversal exhausted.
};

struct VXGISDFSurface
{
    float2 uv;
    float3 geometricNormal;
    float3 shadingNormal;
    float3 tangent;
    float3 bitangent;
    uint materialId;
    uint instanceId;
    uint backFace;
};

uint VXGISDFFlatIndex(uint3 coordinate, uint3 dimensions)
{
    return coordinate.x + dimensions.x * (coordinate.y + dimensions.y * coordinate.z);
}

// Distance samples are exact unsigned surfaceTriangle distances at grid nodes. Their
// Lipschitz lower bound, rather than trilinear interpolation, drives safe steps.
float VXGISDFConservativeDistance(float3 position, VXGISDFGrid grid, StructuredBuffer<float> nodes)
{
    uint3 node = uint3(clamp(floor((position - grid.origin) / grid.cellSize + 0.5),
        0.0, float3(grid.cellDimensions)));
    float distanceAtNode = nodes[VXGISDFFlatIndex(node, grid.cellDimensions + 1)];
    float offset = length(position - (grid.origin + float3(node) * grid.cellSize));
    // Account for float storage/query rounding before taking a sphere step.
    float roundoff = max(grid.cellSize * 1e-4, abs(distanceAtNode) * 2e-6);
    return max(0.0, distanceAtNode - offset - roundoff);
}

// Interpolation is useful for displaying a continuous distance slice only.
float VXGISDFDisplayDistance(float3 position, VXGISDFGrid grid, StructuredBuffer<float> nodes)
{
    float3 coordinate = clamp((position - grid.origin) / grid.cellSize, 0.0, float3(grid.cellDimensions));
    uint3 cell = min(uint3(floor(coordinate)), grid.cellDimensions - 1);
    float3 f = coordinate - float3(cell);
    uint3 dims = grid.cellDimensions + 1;
    float d000 = nodes[VXGISDFFlatIndex(cell, dims)];
    float d100 = nodes[VXGISDFFlatIndex(cell + uint3(1, 0, 0), dims)];
    float d010 = nodes[VXGISDFFlatIndex(cell + uint3(0, 1, 0), dims)];
    float d110 = nodes[VXGISDFFlatIndex(cell + uint3(1, 1, 0), dims)];
    float d001 = nodes[VXGISDFFlatIndex(cell + uint3(0, 0, 1), dims)];
    float d101 = nodes[VXGISDFFlatIndex(cell + uint3(1, 0, 1), dims)];
    float d011 = nodes[VXGISDFFlatIndex(cell + uint3(0, 1, 1), dims)];
    float d111 = nodes[VXGISDFFlatIndex(cell + uint3(1, 1, 1), dims)];
    return lerp(lerp(lerp(d000, d100, f.x), lerp(d010, d110, f.x), f.y),
        lerp(lerp(d001, d101, f.x), lerp(d011, d111, f.x), f.y), f.z);
}

bool VXGISDFRayBounds(float3 rayOrigin, float3 rayDirection, VXGISDFGrid grid,
    float rayMin, float rayMax, out float entry, out float exit)
{
    entry = rayMin;
    exit = rayMax;
    [unroll] for (uint axis = 0; axis < 3; ++axis)
    {
        if (rayDirection[axis] == 0.0)
        {
            if (rayOrigin[axis] < grid.origin[axis] || rayOrigin[axis] > grid.boundsMaximum[axis])
                return false;
        }
        else
        {
            float a = (grid.origin[axis] - rayOrigin[axis]) / rayDirection[axis];
            float b = (grid.boundsMaximum[axis] - rayOrigin[axis]) / rayDirection[axis];
            entry = max(entry, min(a, b));
            exit = min(exit, max(a, b));
        }
    }
    return entry <= exit;
}

bool VXGISDFRayTriangle(float3 rayOrigin, float3 rayDirection, VXGISDFTriangle surfaceTriangle,
    out float distance, out float3 barycentric)
{
    distance = 0.0;
    barycentric = 0.0;
    float3 edge1 = surfaceTriangle.p1 - surfaceTriangle.p0;
    float3 edge2 = surfaceTriangle.p2 - surfaceTriangle.p0;
    float3 p = cross(rayDirection, edge2);
    float determinant = dot(edge1, p);
    // Scale the parallel test by actual triangle area, also for narrow triangles.
    if (abs(determinant) <= 1e-12 * length(cross(edge1, edge2))) return false;
    float inverseDeterminant = rcp(determinant);
    float3 offset = rayOrigin - surfaceTriangle.p0;
    float u = dot(offset, p) * inverseDeterminant;
    float3 q = cross(offset, edge1);
    float v = dot(rayDirection, q) * inverseDeterminant;
    // Boundary ownership is conservative, including shared surfaceTriangle edges.
    if (u < -2e-5 || v < -2e-5 || u + v > 1.00002) return false;
    distance = dot(edge2, q) * inverseDeterminant;
    barycentric = float3(1.0 - u - v, u, v);
    return true;
}

// Dense-cell BVH bounds include the triangle test's barycentric tolerance.
// Zero direction components use an explicit parallel slab test.
bool VXGISDFRayCandidateBounds(float3 rayOrigin, float3 rayDirection,
    float3 minimum, float3 maximum, float rayMin, float rayMax)
{
    float enter = rayMin;
    float exit = rayMax;
    [unroll] for (uint axis = 0; axis < 3; ++axis)
    {
        if (rayDirection[axis] == 0.0)
        {
            if (rayOrigin[axis] < minimum[axis] || rayOrigin[axis] > maximum[axis]) return false;
        }
        else
        {
            float first = (minimum[axis] - rayOrigin[axis]) / rayDirection[axis];
            float second = (maximum[axis] - rayOrigin[axis]) / rayDirection[axis];
            enter = max(enter, min(first, second));
            exit = min(exit, max(first, second));
        }
    }
    return enter <= exit;
}

void VXGISDFInitializeDDA(float3 rayOrigin, float3 rayDirection, float distance,
    VXGISDFGrid grid, float epsilon, out int3 cell, out int3 step,
    out float3 boundaryDistance, out float3 deltaDistance)
{
    float3 p = rayOrigin + rayDirection * distance;
    float3 coordinate = (p - grid.origin) / grid.cellSize;
    [unroll] for (uint axis = 0; axis < 3; ++axis)
    {
        step[axis] = rayDirection[axis] > 0.0 ? 1 : (rayDirection[axis] < 0.0 ? -1 : 0);
        float index = floor(coordinate[axis]);
        // Only exact boundaries change ownership. A forward epsilon here could
        // skip a short initial cell segment and a thin surface within it.
        if (step[axis] < 0 && coordinate[axis] == index) index -= 1.0;
        cell[axis] = int(clamp(index, 0.0, float(grid.cellDimensions[axis] - 1)));
        if (step[axis] == 0)
        {
            boundaryDistance[axis] = 1e30;
            deltaDistance[axis] = 1e30;
        }
        else
        {
            float boundary = grid.origin[axis] + (cell[axis] + (step[axis] > 0 ? 1 : 0)) * grid.cellSize;
            boundaryDistance[axis] = (boundary - rayOrigin[axis]) / rayDirection[axis];
            deltaDistance[axis] = grid.cellSize / abs(rayDirection[axis]);
        }
    }
}

// Coverage is independent of radiance. Opaque surfaces ignore alpha entirely.
// 0: hole, 1: accepted surface, 2: invalid coverage, which must fail closed.
uint VXGISDFCandidateCoverage(VXGISDFTriangle surfaceTriangle, float3 barycentric,
    StructuredBuffer<VXGIMaterialData> materials, uint materialCount,
    SamplerState materialSampler, float materialMipLevel)
{
    if ((surfaceTriangle.flags & 1u) == 0) return 1;
    if (surfaceTriangle.materialId >= materialCount || !all(isfinite(barycentric))
        || !isfinite(materialMipLevel) || materialMipLevel < 0.0) return 2;
    float2 uv = surfaceTriangle.uv0 * barycentric.x + surfaceTriangle.uv1 * barycentric.y
        + surfaceTriangle.uv2 * barycentric.z;
    if (!all(isfinite(uv))) return 2;
    VXGIMaterialData material = materials[surfaceTriangle.materialId];
    if (!isfinite(material.baseColorFactor.a)) return 2;
    Texture2D<float4> albedoTexture = ResourceDescriptorHeap[NonUniformResourceIndex(material.albedoIdx)];
    float sampledAlpha = albedoTexture.SampleLevel(materialSampler, uv, materialMipLevel).a;
    float alpha = sampledAlpha * material.baseColorFactor.a;
    if (!isfinite(sampledAlpha) || !isfinite(alpha)) return 2;
    return alpha >= 0.5 ? 1u : 0u;
}

#ifdef VXGI_MESH_SDF
#include "VXGIMeshSDFCommon.hlsli"
#endif

// Geometry-only diagnostic/golden overload. Runtime GI uses the coverage-aware
// overload below; CPU node distances and CSR still describe complete triangles.
bool VXGISDFTrace(float3 rayOrigin, float3 rayDirection, float rayMin, float rayMax,
    VXGISDFGrid grid, StructuredBuffer<float> nodes, StructuredBuffer<uint> cellOffsets,
    StructuredBuffer<uint> triangleReferences, StructuredBuffer<VXGISDFTriangle> triangles,
    out VXGISDFHit hit)
{
#ifdef VXGI_MESH_SDF
#include "VXGIMeshSDFTraceBody.hlsli"
#else
#include "VXGISDFTraceBody.hlsli"
#endif
}

// Candidate acceptance happens inside the shared traversal, before closest-hit
// selection. Rejected holes never advance rayMin or skip coplanar opaque layers.
// Full-triangle unsigned distances remain a conservative lower bound for this
// subset of covered surfaces; alpha changes do not require rebaking the SDF.
bool VXGISDFTrace(float3 rayOrigin, float3 rayDirection, float rayMin, float rayMax,
    VXGISDFGrid grid, StructuredBuffer<float> nodes, StructuredBuffer<uint> cellOffsets,
    StructuredBuffer<uint> triangleReferences, StructuredBuffer<VXGISDFTriangle> triangles,
    StructuredBuffer<VXGIMaterialData> materials, uint materialCount,
    SamplerState materialSampler, float materialMipLevel, out VXGISDFHit hit)
{
#define VXGI_SDF_TRACE_COVERAGE 1
#ifdef VXGI_MESH_SDF
#include "VXGIMeshSDFTraceBody.hlsli"
#else
#include "VXGISDFTraceBody.hlsli"
#endif
#undef VXGI_SDF_TRACE_COVERAGE
}

uint VXGISDFHitMaterialId(VXGISDFHit hit, StructuredBuffer<VXGISDFTriangle> triangles)
{
    VXGISDFTriangle surfaceTriangle = triangles[hit.triangleIndex];
#ifdef VXGI_MESH_SDF
    VXGIMeshSDFInstance instance = VXGIMeshSDFInstances[hit.instanceIndex];
    return surfaceTriangle.pad0 < instance.materialCount
        ? VXGIMeshSDFMaterials[instance.materialOffset + surfaceTriangle.pad0] : 0xffffffffu;
#else
    return surfaceTriangle.materialId;
#endif
}

uint VXGISDFHitIsCutout(VXGISDFHit hit, StructuredBuffer<VXGISDFTriangle> triangles)
{
#ifdef VXGI_MESH_SDF
    return VXGIMeshSDFInstances[hit.instanceIndex].flags & 1u;
#else
    return triangles[hit.triangleIndex].flags & 1u;
#endif
}

VXGISDFSurface VXGISDFResolveSurface(VXGISDFHit hit, StructuredBuffer<VXGISDFTriangle> triangles)
{
    VXGISDFTriangle surfaceTriangle = triangles[hit.triangleIndex];
    float3 b = hit.barycentric;
    VXGISDFSurface surface;
    surface.uv = surfaceTriangle.uv0 * b.x + surfaceTriangle.uv1 * b.y + surfaceTriangle.uv2 * b.z;
    surface.geometricNormal = hit.geometricNormal;
    float3 n = surfaceTriangle.shadingNormal0 * b.x + surfaceTriangle.shadingNormal1 * b.y + surfaceTriangle.shadingNormal2 * b.z;
    float3 t = surfaceTriangle.tangent0 * b.x + surfaceTriangle.tangent1 * b.y + surfaceTriangle.tangent2 * b.z;
    float3 bitangent = surfaceTriangle.bitangent0 * b.x + surfaceTriangle.bitangent1 * b.y + surfaceTriangle.bitangent2 * b.z;
#ifdef VXGI_MESH_SDF
    VXGIMeshSDFInstance instance = VXGIMeshSDFInstances[hit.instanceIndex];
    n = mul(float4(n, 0), instance.normalFromLocal).xyz;
    t = mul(float4(t, 0), instance.normalFromLocal).xyz;
    bitangent = mul(float4(bitangent, 0), instance.normalFromLocal).xyz;
#endif
    n = dot(n, n) > 1e-12 ? normalize(n) : normalize(hit.geometricNormal);
    t -= n * dot(n, t);
    if (dot(t, t) < 1e-12)
        t = cross(abs(n.z) < 0.999 ? float3(0, 0, 1) : float3(0, 1, 0), n);
    if (dot(bitangent, bitangent) < 1e-12) bitangent = cross(n, t);
    BuildOrthonormalTangentBasis(n, t, bitangent, surface.shadingNormal, surface.tangent, surface.bitangent);
    surface.materialId = VXGISDFHitMaterialId(hit, triangles);
#ifdef VXGI_MESH_SDF
    surface.instanceId = instance.instanceId;
#else
    surface.instanceId = surfaceTriangle.instanceId;
#endif
    surface.backFace = hit.backFace;
    return surface;
}

#endif
