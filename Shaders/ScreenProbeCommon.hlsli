#ifndef SCREEN_PROBE_COMMON_HLSLI
#define SCREEN_PROBE_COMMON_HLSLI

#include "SSGICommon.hlsli"

// Placement/query structure adapted from UE 5.8.4 LumenScreenProbeGather.usf:
// Mark -> Spawn, eight candidates, tile lists and replacement of weak corners.
// Geometry weights retain this renderer's symmetric plane/normal rejection.
#define SP_ADAPTIVE_SAMPLES 8u
#define SP_INVALID_PROBE 0xffffffffu
#define SP_MIN_INTERPOLATION_WEIGHT 0.01

struct ScreenProbeQuery
{
    uint2 sceneSize, uniformSize, atlasSize;
    uint factor, positionIndex, normalIndex, receiverIndex, shIndex;
    uint capacity, countIndex, headerIndex, indicesIndex;
};

uint SPUniformCount(ScreenProbeQuery q) { return q.uniformSize.x * q.uniformSize.y; }

uint SPAdaptiveCount(ScreenProbeQuery q)
{
    if (q.capacity == 0u) return 0u;
    Texture2D<uint> count = ResourceDescriptorHeap[q.countIndex];
    return min(count.Load(int3(0, 0, 0)), q.capacity);
}

uint2 SPAtlasCoord(uint id, uint2 atlasSize)
{
    return uint2(id % atlasSize.x, id / atlasSize.x);
}

uint SPTileCount(ScreenProbeQuery q, int2 tile)
{
    if (q.capacity == 0u || any(tile < 0) || any(tile >= int2(q.uniformSize))) return 0u;
    Texture2D<uint> header = ResourceDescriptorHeap[q.headerIndex];
    return min(header.Load(int3(tile, 0)), SP_ADAPTIVE_SAMPLES);
}

uint SPTileProbe(ScreenProbeQuery q, int2 tile, uint slot)
{
    Texture2D<uint> indices = ResourceDescriptorHeap[q.indicesIndex];
    uint id = indices.Load(int3(tile.x * int(SP_ADAPTIVE_SAMPLES) + int(slot), tile.y, 0));
    return id >= SPUniformCount(q) && id < SPUniformCount(q) + SPAdaptiveCount(q) ? id : SP_INVALID_PROBE;
}

bool SPNormalizeDirection(float3 direction, out float3 unitDirection)
{
    unitDirection = 0;
    float maximum = max(abs(direction.x), max(abs(direction.y), abs(direction.z)));
    if (!all(isfinite(direction)) || maximum == 0) return false;
    float3 scaled = direction / maximum;
    float lengthSquared = dot(scaled, scaled);
    if (!isfinite(lengthSquared) || lengthSquared <= 0) return false;
    unitDirection = scaled * rsqrt(lengthSquared);
    return all(isfinite(unitDirection));
}

float3 SPGeometricNormal(uint2 pixel, float3 shading, uint depthIndex, uint2 size, float4x4 inverseVP)
{
    Texture2D<float> depth = ResourceDescriptorHeap[depthIndex];
    int2 p = int2(pixel), last = int2(size) - 1;
    int2 l = max(p - int2(1, 0), 0), r = min(p + int2(1, 0), last);
    int2 u = max(p - int2(0, 1), 0), d = min(p + int2(0, 1), last);
    float z = depth.Load(int3(p, 0));
    float zl = depth.Load(int3(l, 0)), zr = depth.Load(int3(r, 0));
    float zu = depth.Load(int3(u, 0)), zd = depth.Load(int3(d, 0));
    float dl = zl < 1 && l.x != p.x ? abs(zl - z) : 1e20;
    float dr = zr < 1 && r.x != p.x ? abs(zr - z) : 1e20;
    float du = zu < 1 && u.y != p.y ? abs(zu - z) : 1e20;
    float dd = zd < 1 && d.y != p.y ? abs(zd - z) : 1e20;
    if (min(dl, dr) == 1e20 || min(du, dd) == 1e20) return shading;
    bool useR = dr <= dl, useD = dd <= du;
    float3 center = ReconstructPosition((p + .5) / size, z, inverseVP);
    float3 x = ReconstructPosition(((useR ? r : l) + .5) / size, useR ? zr : zl, inverseVP);
    float3 y = ReconstructPosition(((useD ? d : u) + .5) / size, useD ? zd : zu, inverseVP);
    float3 geometric;
    if (!SPNormalizeDirection(cross(useR ? x - center : center - x, useD ? y - center : center - y), geometric)) return shading;
    float agreement = dot(geometric, shading);
    // At a one-pixel silhouette both derivative neighbors can be background.
    // UE placement uses the GBuffer material normal. Retain our geometric frame
    // on reliable surfaces, and consistently use that material normal when the
    // reconstructed plane contradicts it (Resolve already required this check).
    if (abs(agreement) < .5) return shading;
    return agreement < 0 ? -geometric : geometric;
}

float SPGeometryWeight(float3 p, float3 n, float3 other, float3 m, float footprint)
{
    if (!all(isfinite(other)) || !all(isfinite(m))) return 0;
    float agreement = saturate(dot(n, m));
    if (agreement < .8) return 0;
    float3 delta = other - p;
    float curvature = .75 * length(delta) * sqrt(saturate(1 - agreement * agreement));
    float error = SymmetricPlaneDistance(delta, n, m) / (max(footprint * .75, .001) + curvature);
    return error < 3 ? pow(agreement, 32) * exp2(-4 * error * error) : 0;
}

float SPProbeGeometryWeight(ScreenProbeQuery q, uint id, float3 p, float3 n, float footprint)
{
    if (id == SP_INVALID_PROBE) return 0;
    uint2 coord = SPAtlasCoord(id, q.atlasSize);
    Texture2D<float4> positions = ResourceDescriptorHeap[q.positionIndex];
    Texture2D<float4> normals = ResourceDescriptorHeap[q.normalIndex];
    float4 other = positions.Load(int3(coord, 0));
    if (!(other.a > 0)) return 0;
    return SPGeometryWeight(p, n, other.xyz, normalize(normals.Load(int3(coord, 0)).xyz), footprint);
}

// UE's adaptive corner footprint; use float subtraction to avoid uint underflow.
float SPAdaptiveSpatialWeight(float2 pixel, uint2 candidate, uint factor)
{
    float2 delta = abs(float2(candidate) - pixel);
    return 1.0 - saturate(min(delta.x, delta.y) / float(factor));
}

float SPAdaptiveWeight(ScreenProbeQuery q, uint id, float2 pixel, float3 p, float3 n, float footprint)
{
    if (id == SP_INVALID_PROBE) return 0;
    Texture2D<uint> receivers = ResourceDescriptorHeap[q.receiverIndex];
    uint packed = receivers.Load(int3(SPAtlasCoord(id, q.atlasSize), 0));
    if (packed == 0u) return 0;
    uint2 receiver = uint2((packed - 1u) % q.sceneSize.x, (packed - 1u) / q.sceneSize.x);
    return SPProbeGeometryWeight(q, id, p, n, footprint) * SPAdaptiveSpatialWeight(pixel, receiver, q.factor);
}

struct ScreenProbeSamples
{
    uint4 ids;
    float4 weights;
};

ScreenProbeSamples SPSelectSamples(ScreenProbeQuery q, float2 pixel, float3 p, float3 n,
    float footprint, bool adaptive)
{
    ScreenProbeSamples result;
    result.ids = SP_INVALID_PROBE;
    result.weights = 0;
    float2 grid = (pixel + .5) / float(q.factor) - .5;
    int2 base = int2(floor(grid));
    float2 fraction = frac(grid);
    [unroll] for (uint i = 0u; i < 4u; ++i)
    {
        int2 offset = int2(i & 1u, i >> 1u), tile = base + offset;
        if (any(tile < 0) || any(tile >= int2(q.uniformSize))) continue;
        uint id = uint(tile.y) * q.uniformSize.x + uint(tile.x);
        float2 bilinear = lerp(1 - fraction, fraction, float2(offset));
        float weight = bilinear.x * bilinear.y * SPProbeGeometryWeight(q, id, p, n, footprint);
        if (adaptive && weight < SP_MIN_INTERPOLATION_WEIGHT)
        {
            uint count = SPTileCount(q, tile);
            [loop] for (uint slot = 0u; slot < count; ++slot)
            {
                uint candidate = SPTileProbe(q, tile, slot);
                float w = SPAdaptiveWeight(q, candidate, pixel, p, n, footprint);
                if (w > weight) { weight = w; id = candidate; }
            }
        }
        result.ids[i] = id;
        result.weights[i] = weight;
    }
    return result;
}

// Orthonormal real SH. Keep projection/convolution and receiver evaluation identical.
void SPBasis(float3 w, out float basis[9])
{
    basis[0] = .2820947918;
    basis[1] = .4886025119 * w.y;
    basis[2] = .4886025119 * w.z;
    basis[3] = .4886025119 * w.x;
    basis[4] = 1.0925484306 * w.x * w.y;
    basis[5] = 1.0925484306 * w.y * w.z;
    basis[6] = .3153915653 * (3 * w.z * w.z - 1);
    basis[7] = 1.0925484306 * w.x * w.z;
    basis[8] = .5462742153 * (w.x * w.x - w.y * w.y);
}

float3 SPReadSH(ScreenProbeQuery q, uint id, float basis[9], out bool fresh)
{
    Texture2D<float4> sh = ResourceDescriptorHeap[q.shIndex];
    Texture2D<float4> positions = ResourceDescriptorHeap[q.positionIndex];
    uint2 coord = SPAtlasCoord(id, q.atlasSize);
    fresh = positions.Load(int3(coord, 0)).a > 1.5;
    float3 result = 0;
    [unroll] for (uint c = 0u; c < 9u; ++c)
        result += sh.Load(int3(coord * 3u + uint2(c % 3u, c / 3u), 0)).rgb * basis[c];
    return max(result, 0);
}

bool SPResolveSH(ScreenProbeQuery q, uint2 pixel, float3 p, float3 geometric, float3 shading,
    float footprint, out float3 lighting, out bool fresh)
{
    float basis[9]; SPBasis(shading, basis);
    ScreenProbeSamples samples = SPSelectSamples(q, pixel, p, geometric, footprint, true);
    float3 sum = 0; float weights = 0;
    fresh = false;
    [unroll] for (uint i = 0u; i < 4u; ++i)
        if (samples.weights[i] > 0)
        {
            bool updated;
            sum += SPReadSH(q, samples.ids[i], basis, updated) * samples.weights[i];
            weights += samples.weights[i]; fresh = fresh || updated;
        }
    if (weights <= 1e-5)
    {
        float2 grid = (pixel + .5) / float(q.factor) - .5;
        int2 nearest = int2(floor(grid + .5));
        [loop] for (int y = -1; y <= 1; ++y)
        [loop] for (int x = -1; x <= 1; ++x)
        {
            int2 tile = nearest + int2(x, y);
            if (any(tile < 0) || any(tile >= int2(q.uniformSize))) continue;
            uint id = uint(tile.y) * q.uniformSize.x + uint(tile.x);
            float w = SPProbeGeometryWeight(q, id, p, geometric, footprint);
            uint count = SPTileCount(q, tile);
            [loop] for (uint slot = 0u; slot < count; ++slot)
            {
                uint other = SPTileProbe(q, tile, slot);
                float otherWeight = SPAdaptiveWeight(q, other, pixel, p, geometric, footprint);
                if (otherWeight > w) { id = other; w = otherWeight; }
            }
            float2 delta = float2(tile) - grid;
            w *= exp2(-dot(delta, delta));
            if (w > 0)
            {
                bool updated; sum += SPReadSH(q, id, basis, updated) * w;
                weights += w; fresh = fresh || updated;
            }
        }
    }
    lighting = weights > 1e-5 ? sum / weights : 0;
    return weights > 1e-5;
}

void SPCounter(uint descriptorPlusOne, uint counter, uint amount)
{
    if (descriptorPlusOne == 0u || amount == 0u) return;
    RWTexture2D<uint> counters = ResourceDescriptorHeap[descriptorPlusOne - 1u];
    InterlockedAdd(counters[uint2(counter, 0)], amount);
}

#endif
