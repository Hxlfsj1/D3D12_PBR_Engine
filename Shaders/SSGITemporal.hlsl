#include "SSGICommon.hlsli"

cbuffer Constants : register(b0)
{
    float4x4 InvViewProj, PreviousViewProj, PreviousInvViewProj;
    uint2 Size, OutputSize;
    uint DepthIndex, NormalIndex, ORMIndex, CurrentIndex;
    uint PreviousGIIndex, PreviousDepthIndex, PreviousNormalIndex, HistoryValid;
    uint OutputGIIndex, OutputDepthIndex, OutputNormalIndex, Padding;
};

float3 WorldPosition(float2 uv, float z, float4x4 inverseMatrix)
{
    return SSGIViewPosition(uv, z, inverseMatrix);
}

bool SameSurface(float3 p, float3 n, float3 q, float3 m, float footprint, float planeTolerance)
{
    float3 delta = q - p;
    return dot(n, m) >= 0.85 && length(delta) <= footprint * 3 &&
        max(abs(dot(delta, n)), abs(dot(delta, m))) <= planeTolerance;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= OutputSize)) return;
    Texture2D<float> depth = ResourceDescriptorHeap[DepthIndex];
    Texture2D<float4> normal = ResourceDescriptorHeap[NormalIndex];
    Texture2D<float4> orm = ResourceDescriptorHeap[ORMIndex];
    Texture2D<float4> current = ResourceDescriptorHeap[CurrentIndex];
    RWTexture2D<float4> output = ResourceDescriptorHeap[OutputGIIndex];
    RWTexture2D<float> outputDepth = ResourceDescriptorHeap[OutputDepthIndex];
    RWTexture2D<float4> outputNormal = ResourceDescriptorHeap[OutputNormalIndex];
    uint2 pixel = id.xy * 2;
    float z = depth.Load(int3(pixel, 0));
    output[id.xy] = 0;
    outputDepth[id.xy] = 1;
    outputNormal[id.xy] = 0;
    if (z >= 1 || orm.Load(int3(pixel, 0)).a < 0.5) return;

    float3 encodedNormal = normal.Load(int3(pixel, 0)).xyz;
    float3 n = normalize(encodedNormal * 2 - 1);
    float3 value = current.Load(int3(id.xy, 0)).rgb;
    outputDepth[id.xy] = z;
    outputNormal[id.xy] = float4(encodedNormal, 1);
    output[id.xy] = float4(value, 1); // Alpha is history length here, not the raw hit fraction.

    if (!HistoryValid) return;

    float2 uv = (pixel + 0.5) / Size;
    float3 p = WorldPosition(uv, z, InvViewProj);
    float footprint = max(length(WorldPosition(uv + float2(1.0 / Size.x, 0), z, InvViewProj) - p),
        length(WorldPosition(uv + float2(0, 1.0 / Size.y), z, InvViewProj) - p));
    footprint = max(footprint, 1e-4);
    float rayDepth = length(p - WorldPosition(uv, 0, InvViewProj));
    float planeTolerance = max(min(footprint * 0.5, rayDepth * 0.01), 0.001);
    float4 previousClip = mul(float4(p, 1), PreviousViewProj);
    if (previousClip.w <= 0) return;
    float3 previousNDC = previousClip.xyz / previousClip.w;
    float2 previousUV = previousNDC.xy * float2(0.5, -0.5) + 0.5;
    if (any(previousUV < 0) || any(previousUV >= 1) || previousNDC.z < 0 || previousNDC.z >= 1) return;
    // History texel i represents full pixel center 2*i+0.5, not the center of a 2x2 block.
    float2 historyPixel = (previousUV * Size - 0.5) * 0.5;
    int2 base = int2(floor(historyPixel));
    float2 fraction = frac(historyPixel);
    Texture2D<float4> previousGI = ResourceDescriptorHeap[PreviousGIIndex];
    Texture2D<float> previousDepth = ResourceDescriptorHeap[PreviousDepthIndex];
    Texture2D<float4> previousNormal = ResourceDescriptorHeap[PreviousNormalIndex];
    float4 history = 0;
    float historyWeight = 0;
    [loop] for (int i = 0; i < 4; ++i)
    {
        int2 offset = int2(i & 1, i >> 1);
        int2 tap = base + offset;
        if (any(tap < 0) || any(tap >= int2(OutputSize))) continue;

        float4 tapNormal = previousNormal.Load(int3(tap, 0));
        if (tapNormal.a < 0.5) continue;
        float tapZ = previousDepth.Load(int3(tap, 0));
        float3 q = WorldPosition((tap * 2 + 0.5) / Size, tapZ, PreviousInvViewProj);
        float3 m = normalize(tapNormal.xyz * 2 - 1);

        if (!SameSurface(p, n, q, m, footprint, planeTolerance)) continue;
        float2 weights = lerp(1 - fraction, fraction, float2(offset));
        float weight = weights.x * weights.y;

        history += previousGI.Load(int3(tap, 0)) * weight;
        historyWeight += weight;
    }
    if (historyWeight < 0.01) return;

    history /= historyWeight;

    // Geometry-compatible current neighborhood supplies a variance clip box for stale lighting.
    float3 sum = value, sumSquared = value * value, low = value, high = value;
    float count = 1;
    [loop] for (int y = -1; y <= 1; ++y)
    [loop] for (int x = -1; x <= 1; ++x)
    {
        if (x == 0 && y == 0) continue;
        int2 tap = int2(id.xy) + int2(x, y);
        if (any(tap < 0) || any(tap >= int2(OutputSize))) continue;
        int2 fullPixel = tap * 2;
        float tapZ = depth.Load(int3(fullPixel, 0));
        if (tapZ >= 1 || orm.Load(int3(fullPixel, 0)).a < 0.5) continue;
        float3 tapN = normalize(normal.Load(int3(fullPixel, 0)).xyz * 2 - 1);
        float3 q = WorldPosition((fullPixel + 0.5) / Size, tapZ, InvViewProj);
        if (!SameSurface(p, n, q, tapN, footprint, planeTolerance)) continue;
        float3 c = current.Load(int3(tap, 0)).rgb;
        sum += c; sumSquared += c * c; low = min(low, c); high = max(high, c); ++count;
    }
    float3 mean = sum / count;
    float3 sigma = sqrt(max(sumSquared / count - mean * mean, 0));
    low = max(low, min(value, mean - 1.5 * sigma));
    high = min(high, max(value, mean + 1.5 * sigma));
    float3 clipped = clamp(history.rgb, low, high);
    float frames = min(history.a, 15);
    // A clipped history has stale lighting; let it adapt promptly without losing geometric validity.
    if (any(abs(clipped - history.rgb) > 1e-3)) { frames = min(frames, 3); }
    float3 result = lerp(value, clipped, frames / (frames + 1));
    output[id.xy] = float4(result, frames + 1);

}
