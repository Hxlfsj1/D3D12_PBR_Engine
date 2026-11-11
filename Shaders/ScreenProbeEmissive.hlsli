#ifndef SCREEN_PROBE_EMISSIVE_HLSLI
#define SCREEN_PROBE_EMISSIVE_HLSLI

#include "GISourceCommon.hlsli"

// These fields belong to the inactive legacy sharing path. Root/CB layouts stay unchanged.
#define SP_EMISSIVE_SOURCE sharedMeshLightingTextureIdx
#define SP_EMISSIVE_LIST_UAV sharedMeshReceiverTextureIdx
#define SP_EMISSIVE_COUNT_UAV sharedGlobalLightingTextureIdx
#define SP_EMISSIVE_LIST_SRV sharedGlobalReceiverTextureIdx
#define SP_EMISSIVE_COUNT_SRV sharedRepairQueueTextureIdx
#define SP_EMISSIVE_SAMPLES sharedMeshSpan

groupshared uint sharedScreenEmissiveCount;

uint LoadScreenEmissiveCount()
{
    if (SP_EMISSIVE_SAMPLES == 0u) return 0u;
    Texture2D<uint> count = ResourceDescriptorHeap[SP_EMISSIVE_COUNT_SRV];
    return count.Load(int3(0, 0, 0));
}

uint ScreenEmissiveCount()
{
    return sharedScreenEmissiveCount;
}

float3 ScreenEmissiveRadiance(uint2 pixel)
{
    Texture2D<float4> emission = ResourceDescriptorHeap[SP_EMISSIVE_SOURCE];
    float3 value = emission.Load(int3(pixel, 0)).rgb;
    return all(isfinite(value)) ? GICapSourceEmissive(max(value, 0)) : 0;
}

[numthreads(1, 1, 1)]
void CSMain_EmissiveClear(uint3 id : SV_DispatchThreadID)
{
    RWTexture2D<uint> count = ResourceDescriptorHeap[SP_EMISSIVE_COUNT_UAV];
    count[uint2(0, 0)] = 0;
}

[numthreads(8, 8, 1)]
void CSMain_EmissiveBuild(uint3 id : SV_DispatchThreadID)
{
    bool valid = false;
    if (all(id.xy < sceneSize))
    {
        Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
        float z = depth.Load(int3(id.xy, 0));
        valid = isfinite(z) && z >= 0 && z < 1 && any(ScreenEmissiveRadiance(id.xy) > 0);
    }
    uint waveCount = WaveActiveCountBits(valid);
    if (waveCount != 0u)
    {
        RWTexture2D<uint> count = ResourceDescriptorHeap[SP_EMISSIVE_COUNT_UAV];
        uint start = 0;
        if (WaveIsFirstLane()) InterlockedAdd(count[uint2(0, 0)], waveCount, start);
        start = WaveReadLaneFirst(start);
        if (valid)
        {
            RWTexture2D<uint> pixels = ResourceDescriptorHeap[SP_EMISSIVE_LIST_UAV];
            uint index = start + WavePrefixCountBits(valid);
            pixels[uint2(index % sceneSize.x, index / sceneSize.x)] = id.y * sceneSize.x + id.x;
        }
    }
}

uint ScreenEmissiveHash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; return x ^ (x >> 16);
}

struct ScreenEmissiveSample
{
    float3 radiance;
    uint cell;
    float distance;
};

// Sample visible emitting pixels, rather than hoping a uniform hemisphere ray
// lands on one. Each pixel represents its local world-space surface area.
// PDF(pixel)=1/count. The area-to-solid-angle Jacobian is |nL.w| / r^2.
// Only the emission term is replaced; ordinary direct diffuse and world fallback
// retain their original estimators and visibility rules.
// Visibility to a known emitting endpoint. Depth thickness is used only for
// conservative candidate selection; actual blockers must intersect the ray's
// per-pixel tangent plane before the endpoint. This avoids treating the light
// or its ceiling thickness shell as an occluder, and blocks both face directions.
bool ScreenEmissiveVisible(uint2 receiver, float3 origin, float3 direction, float distance, float endpointTolerance)
{
    float3 start = Project(origin);
    float3 delta = Project(origin + direction * distance) - start;
    if (!all(isfinite(start)) || !all(isfinite(delta)) || origin.z <= 0 ||
        any(start.xy < 0) || any(start.xy >= 1)) return false;
    float endT = 1;
    [unroll] for (uint axis = 0; axis < 2; ++axis)
    {
        if (delta[axis] > 0) endT = min(endT, (1 - start[axis]) / delta[axis]);
        if (delta[axis] < 0) endT = min(endT, -start[axis] / delta[axis]);
    }
    if (endT < 1.0 - 1e-5) return false;
    float2 startPixels = start.xy * sceneSize, deltaPixels = delta.xy * sceneSize;
    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    Texture2D<float4> normals = ResourceDescriptorHeap[normalTextureIdx];
    uint mip = 0;
    float t = 0;
    [loop] for (uint iteration = 0; iteration < maxIterations * 4u && t < endT; ++iteration)
    {
        float2 address = startPixels + deltaPixels * t;
        float2 nudge = max(float2(.001, .001), abs(address) * 9.53674316e-7);
        int2 cell = int2(floor((address + sign(deltaPixels) * nudge) / float(1u << mip)));
        uint cellSize = 1u << mip;
        if (any(cell < 0) || any(cell >= int2((sceneSize + cellSize - 1) / cellSize))) return false;
        float exitT = CellExit(startPixels, deltaPixels, cell, mip, endT);
        if (mip > 0)
        {
            Texture2D<float2> hierarchy = ResourceDescriptorHeap[NonUniformResourceIndex(levels[mip - 1].x)];
            float2 bounds = hierarchy.Load(int3(cell, 0));
            if (bounds.x <= bounds.y)
            {
                // Pad by one pixel's depth span, rather than a fixed thick shell.
                // Fine cells are tested as finite tangent planes below.
                float padding = max(.001, (bounds.y - bounds.x) / float(cellSize - 1u));
                float nearDepth = DeviceDepth(max(bounds.x - padding, .001));
                float farDepth = DeviceDepth(bounds.y + padding);
                float entryT = t;
                bool potential;
                if (abs(delta.z) < 1e-8) potential = start.z >= nearDepth && start.z <= farDepth;
                else
                {
                    float x = (nearDepth - start.z) / delta.z, y = (farDepth - start.z) / delta.z;
                    entryT = max(t, min(x, y));
                    potential = entryT <= min(exitT, max(x, y));
                }
                if (potential) { t = entryT; --mip; continue; }
            }
        }
        else if (any(uint2(cell) != receiver))
        {
            float z = depth.Load(int3(cell, 0));
            if (z >= 0 && z < 1)
            {
                float3 worldNormal = SSGIReceiverGeometricNormal(uint2(cell),
                    normalize(DecodeGBufferNormal(normals.Load(int3(cell, 0)).xyz)));
                float3 normal = normalize(mul(float4(worldNormal, 0), viewMat).xyz);
                float3 planePoint = ReconstructPosition((cell + .5) / sceneSize, z, invProjMat);
                if (!all(isfinite(normal)) || !all(isfinite(planePoint))) return false;
                float denominator = dot(normal, direction);
                if (abs(denominator) > 1e-6)
                {
                    float hitDistance = dot(planePoint - origin, normal) / denominator;
                    if (hitDistance > 0 && hitDistance < distance - endpointTolerance)
                    {
                        float2 pixel = Project(origin + direction * hitDistance).xy * sceneSize;
                        if (all(pixel >= float2(cell) - .001) && all(pixel <= float2(cell + 1) + .001)) return false;
                    }
                }
            }
        }
        t = exitT;
        mip = min(mip + 1, levelCount);
    }
    return t >= endT; // Budget exhaustion is unresolved, never a visibility proof.
}

ScreenEmissiveSample SampleScreenEmissive(uint2 probe, uint2 receiver, uint lane,
    float3 p, float3 launchNormal, float3 normal)
{
    ScreenEmissiveSample result = (ScreenEmissiveSample)0;
    result.cell = 0xffffffffu;
    result.distance = hybrid.worldMaxTraceDistance;
    if (lane >= SP_EMISSIVE_SAMPLES) return result;
    uint count = ScreenEmissiveCount();
    if (count == 0u) return result;
    uint seed = ScreenEmissiveHash(probe.x + probe.y * outputSize.x +
        frameIndex * 0x9e3779b9u + lane * 0x85ebca6bu);
    // Multiply-high mapping using 32-bit arithmetic (no shader-int64 requirement).
    uint lo = seed & 0xffffu, hi = seed >> 16;
    uint t = hi * (count & 0xffffu) + ((lo * (count & 0xffffu)) >> 16);
    uint selected = hi * (count >> 16) + (t >> 16) +
        (((t & 0xffffu) + lo * (count >> 16)) >> 16);
    Texture2D<uint> list = ResourceDescriptorHeap[SP_EMISSIVE_LIST_SRV];
    uint packed = list.Load(int3(uint2(selected % sceneSize.x, selected / sceneSize.x), 0));
    uint2 pixel = uint2(packed % sceneSize.x, packed / sceneSize.x);
    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    Texture2D<float4> normals = ResourceDescriptorHeap[normalTextureIdx];
    float z = depth.Load(int3(pixel, 0));
    float2 uv = (pixel + 0.5) / sceneSize;
    float3 target = ReconstructPosition(uv, z, invViewProjMat);
    float3 lightNormal = SSGIReceiverGeometricNormal(pixel,
        normalize(DecodeGBufferNormal(normals.Load(int3(pixel, 0)).xyz)));
    float3 camera = mul(float4(0, 0, 0, 1), invViewMat).xyz;
    float3 dxRay = ReconstructPosition(uv + float2(1.0 / sceneSize.x, 0), z, invViewProjMat) - camera;
    float3 dyRay = ReconstructPosition(uv + float2(0, 1.0 / sceneSize.y), z, invViewProjMat) - camera;
    float plane = dot(lightNormal, target - camera);
    float denomX = dot(lightNormal, dxRay), denomY = dot(lightNormal, dyRay);
    if (abs(denomX) < 1e-6 || abs(denomY) < 1e-6) return result;
    float3 dx = camera + dxRay * (plane / denomX) - target;
    float3 dy = camera + dyRay * (plane / denomY) - target;
    float area = length(cross(dx, dy));
    float3 origin = p + launchNormal * normalBias;
    float3 toLight = target - origin;
    float distance = length(toLight);
    if (!all(isfinite(target)) || !isfinite(area) || area <= 0 ||
        !isfinite(distance) || distance < 0.001 || distance > radius) return result;
    float3 direction = toLight / distance;
    normal = normalize(normal);
    float mu = dot(normal, direction);
    if (mu <= 0) return result;
    float3 viewOrigin = mul(float4(origin, 1), viewMat).xyz;
    float3 viewDirection = normalize(mul(float4(direction, 0), viewMat).xyz);
    if (dot(lightNormal, -direction) <= 0) return result;
    float tolerance = max(.001, .25 * max(length(dx), length(dy)));
    if (!ScreenEmissiveVisible(receiver, viewOrigin, viewDirection, distance, tolerance)) return result;
    float3 emission = ScreenEmissiveRadiance(pixel);
    if (!any(emission > 0)) return result;
    float cosine = abs(dot(lightNormal, -direction));
    float solidAngleWeight = float(count) * area * cosine / (distance * distance);
    float3 value = emission * (solidAngleWeight * (64.0 / 6.28318530718) / float(SP_EMISSIVE_SAMPLES));
    if (!all(isfinite(value))) return result;
    float3 tangent = normalize(cross(abs(normal.z) < 0.999 ? float3(0, 0, 1) : float3(0, 1, 0), normal));
    float phi = atan2(dot(direction, cross(normal, tangent)), dot(direction, tangent));
    if (phi < 0) phi += 6.28318530718;
    uint2 cell = min(uint2(mu * 8.0, phi * (8.0 / 6.28318530718)), 7u);
    result.radiance = value;
    result.cell = cell.y * 8u + cell.x;
    result.distance = distance;
    return result;
}

groupshared float3 sharedEmissiveRadiance[64];
groupshared uint sharedEmissiveCell[64];
groupshared float sharedEmissiveDistance[64];

#endif
