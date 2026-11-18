#ifndef SCREEN_PROBE_EMISSIVE_HLSLI
#define SCREEN_PROBE_EMISSIVE_HLSLI

#include "GISourceCommon.hlsli"

// Screen-emitter sampling descriptors and budget.
#define SP_EMISSIVE_SOURCE emissiveSourceIndex
#define SP_EMISSIVE_LIST_UAV emissiveListUavIndex
#define SP_EMISSIVE_COUNT_UAV emissiveCountUavIndex
#define SP_EMISSIVE_LIST_SRV emissiveListIndex
#define SP_EMISSIVE_COUNT_SRV emissiveCountIndex
#define SP_EMISSIVE_SAMPLES emissiveSamples

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
        Texture2D<float4> orm=ResourceDescriptorHeap[ormTextureIdx];
        valid = orm.Load(int3(id.xy,0)).a>=.5 && isfinite(z) && z >= 0 && z < 1 && any(ScreenEmissiveRadiance(id.xy) > 0);
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

// Sample visible emitting pixels, rather than hoping a uniform hemisphere ray
// lands on one. Each pixel represents its local world-space surface area.
// PDF(pixel)=1/count. The area-to-solid-angle Jacobian is |nL.w| / r^2.
// Only the emission term is replaced; ordinary reflected screen radiance keeps
// its hemisphere estimator and screen visibility rules.
ProbeEmissiveSample SampleScreenEmissive(uint2 samplingKey, uint2 receiver, uint lane,
    float3 p, float3 launchNormal, float3 normal)
{
    ProbeEmissiveSample result = (ProbeEmissiveSample)0;
    result.cell = 0xffffffffu;
    result.distance = radius;
    if (lane >= SP_EMISSIVE_SAMPLES) return result;
    uint count = ScreenEmissiveCount();
    if (count == 0u) return result;
    // A matched adaptive receiver keeps its key when its compact atlas ID changes.
    uint seed = ProbeRandomHash(samplingKey.x + samplingKey.y * outputSize.x +
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
    SurfaceCacheSample cached;
    if(!SSGIReadCache(target,lightNormal,0xffffffffu,.001,cached))return result;
    float3 emission=cached.sourceEmission;
    if (!any(emission > 0)) return result;
    float cosine = abs(dot(lightNormal, -direction));
    float solidAngleWeight = float(count) * area * cosine / (distance * distance);
    float3 value = emission * (solidAngleWeight * (64.0 / 6.28318530718) / float(SP_EMISSIVE_SAMPLES));
    if (!all(isfinite(value))) return result;
    float3 tangent = normalize(cross(abs(normal.z) < 0.999 ? float3(0, 0, 1) : float3(0, 1, 0), normal));
    float phi = atan2(dot(direction, cross(normal, tangent)), dot(direction, tangent));
    if (phi < 0) phi += 6.28318530718;
    uint2 cell = min(uint2(mu * 8.0, phi * (8.0 / 6.28318530718)), 7u);
    SPCounter(probeDiagnosticsIndexPlusOne,21u,1u);
    result.radiance = value;
    result.cell = cell.y * 8u + cell.x;
    result.distance = distance;
    return result;
}

#endif
