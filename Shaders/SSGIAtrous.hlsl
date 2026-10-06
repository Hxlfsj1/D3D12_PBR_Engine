#include "SSGICommon.hlsli"
#include "../ThirdParty/NVIDIA/NRD/RelaxDiffuse.hlsli"
#include "../ThirdParty/NVIDIA/NRD/RelaxAtrous.hlsli"
#ifndef SSGI_ATROUS_FIRST
#define SSGI_ATROUS_FIRST 0
#endif
cbuffer Constants : register(b0)
{
    float4x4 View, InvProj;
    uint2 Size, OutputSize;
    uint GIIndex, DepthIndex, NormalIndex, HistoryIndex;
    uint OutputIndex, Step;
    float PhiLuminance, DepthThreshold;
};
// Both variants require at most a two-texel border (short-history 5x5 or step-2 3x3).
groupshared float4 SignalTile[12][12];
groupshared float4 NormalTile[12][12]; // view normal, history length (0 means invalid)
groupshared float3 PositionTile[12][12];

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID,
    uint3 thread : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    Texture2D<float4> gi = ResourceDescriptorHeap[GIIndex];
    Texture2D<float> depth = ResourceDescriptorHeap[DepthIndex];
    Texture2D<float4> normal = ResourceDescriptorHeap[NormalIndex];
    Texture2D<float4> history = ResourceDescriptorHeap[HistoryIndex];
    RWTexture2D<float4> output = ResourceDescriptorHeap[OutputIndex];
    for (uint i = lane; i < 144; i += 64)
    {
        uint2 local = uint2(i % 12, i / 12);
        int2 pixel = clamp(int2(group.xy * 8 + local) - 2, 0, int2(OutputSize) - 1);
        int2 fullPixel = pixel * 2;
        float length = history.Load(int3(pixel, 0)).a;
        float3 n = 0, p = 0;
        if (length > 0)
        {
            n = normalize(mul(float4(normal.Load(int3(fullPixel, 0)).xyz * 2 - 1, 0), View).xyz);
            p = SSGIViewPosition((fullPixel + 0.5) / Size, depth.Load(int3(fullPixel, 0)), InvProj);
        }
        SignalTile[local.y][local.x] = gi.Load(int3(pixel, 0));
        NormalTile[local.y][local.x] = float4(n, length);
        PositionTile[local.y][local.x] = p;
    }
    GroupMemoryBarrierWithGroupSync();
    if (any(id.xy >= OutputSize)) return;
    uint2 center = thread.xy + 2;
    float4 centerNormal = NormalTile[center.y][center.x];
    if (centerNormal.a == 0) { output[id.xy] = 0; return; }
    float3 p = PositionTile[center.y][center.x];
    float threshold = max(DepthThreshold * abs(p.z), 1e-6);
    float4 value = SignalTile[center.y][center.x];
    float centerL = RelaxLuminance(value.rgb);
    float variance = value.a;
    bool shortHistory = false;
#if SSGI_ATROUS_FIRST
    // RELAX: smooth color + second moment before deriving the variance used by edge stopping.
    float4 smoothed = 0; float smoothWeight = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        int2 tap = int2(center) + int2(x, y);
        if (NormalTile[tap.y][tap.x].a > 0)
        {
            float k = (x == 0 ? 0.5 : 0.25) * (y == 0 ? 0.5 : 0.25);
            smoothed += SignalTile[tap.y][tap.x] * k; smoothWeight += k;
        }
    }
    smoothed /= smoothWeight;
    float mean = RelaxLuminance(smoothed.rgb);
    variance = max(0, smoothed.a - mean * mean);
    shortHistory = centerNormal.a < 3;
#endif
    // NRD defaults: diffuse phi=2, lobe fraction=.5. Relax normals while history is young.
    float fraction = lerp(0.99, 0.5 / sqrt(float(Step)), saturate(centerNormal.a / 5.0));
    if (shortHistory) fraction = 0.5;
    float inverseSigma = rcp(max(1e-4, PhiLuminance * sqrt(max(variance, 0))));
    int radius = shortHistory ? 2 : 1;
    float3 sum = 0;
    float momentOrVariance = 0, sumWeight = 0;
    [loop] for (int y = -radius; y <= radius; ++y)
    [loop] for (int x = -radius; x <= radius; ++x)
    {
        int2 offset = int2(x, y) * int(Step);
        int2 pixel = int2(id.xy) + offset;
        if (any(pixel < 0) || any(pixel >= int2(OutputSize))) continue;
        int2 tap = int2(center) + offset;
        float4 tapNormal = NormalTile[tap.y][tap.x];
        if (tapNormal.a == 0) continue;
        float4 c = SignalTile[tap.y][tap.x];
        float kernel = shortHistory ? 1 : (x == 0 ? 0.44198 : 0.27901) * (y == 0 ? 0.44198 : 0.27901);
        float w = kernel * RelaxAtrousNormalWeight(centerNormal.xyz, tapNormal.xyz, fraction);
        w *= RelaxAtrousPlaneWeight(p, centerNormal.xyz, PositionTile[tap.y][tap.x], threshold);
        if (!shortHistory) w *= exp(-abs(centerL - RelaxLuminance(c.rgb)) * inverseSigma);
        sum += c.rgb * w;
#if SSGI_ATROUS_FIRST
        // The first pass averages second moments; only then convert to variance.
        momentOrVariance += c.a * w;
#else
        // Later passes propagate variance using squared weights, not ordinary color weights.
        momentOrVariance += max(c.a, 0) * w * w;
#endif
        sumWeight += w;
    }
    float3 result = sum / sumWeight; // Valid center always contributes a positive weight.
#if SSGI_ATROUS_FIRST
    float luma = RelaxLuminance(result);
    float outVariance = max(0, momentOrVariance / sumWeight - luma * luma);
    if (shortHistory) outVariance *= max(1.0, 4.0 / (centerNormal.a + 1.0));
#else
    float outVariance = momentOrVariance / (sumWeight * sumWeight);
#endif
    output[id.xy] = float4(result, outVariance);
}
