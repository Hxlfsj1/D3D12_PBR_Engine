#include "SSGICommon.hlsli"
#include "../ThirdParty/NVIDIA/NRD/RelaxDiffuse.hlsli"

cbuffer Constants : register(b0)
{
    float4x4 InvViewProj, PreviousViewProj, PreviousInvViewProj;
    uint2 Size, OutputSize;
    uint DepthIndex, NormalIndex, ORMIndex, CurrentIndex;
    uint PreviousGIIndex, PreviousDepthIndex, PreviousNormalIndex, HistoryValid;
    uint OutputGIIndex, OutputDepthIndex, OutputNormalIndex, PreviousFastIndex;
    uint OutputFastIndex;
    float MaxHistoryFrames, MaxFastAccumulatedFrames;
    uint Padding;
};

float3 WorldPosition(float2 uv, float z, float4x4 inverseMatrix)
{
    return SSGIViewPosition(uv, z, inverseMatrix);
}

bool SameSurface(float3 p, float3 n, float3 q, float3 m, float planeTolerance)
{
    float3 delta = q - p;
    // Reprojection already restricts taps to the previous bilinear footprint.
    // At grazing angles coplanar taps can be far apart along the surface;
    // reject normal/plane disagreement, not that tangential separation.
    return dot(n, m) >= 0.85 &&
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
    RWTexture2D<float4> outputFast = ResourceDescriptorHeap[OutputFastIndex];
    RWTexture2D<float4> output = ResourceDescriptorHeap[OutputGIIndex];
    RWTexture2D<float> outputDepth = ResourceDescriptorHeap[OutputDepthIndex];
    RWTexture2D<float4> outputNormal = ResourceDescriptorHeap[OutputNormalIndex];
    uint2 pixel = id.xy * 2;
    float z = depth.Load(int3(pixel, 0));
    output[id.xy] = 0;
    outputFast[id.xy] = 0;
    outputDepth[id.xy] = 1;
    outputNormal[id.xy] = 0;
    if (z >= 1 || orm.Load(int3(pixel, 0)).a < 0.5) return;

    float3 encodedNormal = normal.Load(int3(pixel, 0)).xyz;
    float3 n = normalize(encodedNormal * 2 - 1);
    float3 value = current.Load(int3(id.xy, 0)).rgb;
    outputDepth[id.xy] = z;
    outputNormal[id.xy] = float4(encodedNormal, 1);
    float luminance = RelaxLuminance(value);
    float moment2 = luminance * luminance;
    outputFast[id.xy] = float4(value, moment2);
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

    Texture2D<float4> previousFast = ResourceDescriptorHeap[PreviousFastIndex];
    float4 fastHistory = 0;
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

        if (!SameSurface(p, n, q, m, planeTolerance)) continue;

        float2 weights = lerp(1 - fraction, fraction, float2(offset));
        float weight = weights.x * weights.y;

        history += previousGI.Load(int3(tap, 0)) * weight;
        fastHistory += previousFast.Load(int3(tap, 0)) * weight;
        historyWeight += weight;
    }

    if (historyWeight < 0.01) return;

    history /= historyWeight;

    fastHistory /= historyWeight;
    float frames = min(history.a + 1, MaxHistoryFrames);
    // RELAX: independent slow/fast accumulation before neighborhood clamping.
    float alpha = 1.0 / frames;
    float fastAlpha = max(1.0 / (MaxFastAccumulatedFrames + 1.0), alpha);
    output[id.xy] = float4(lerp(history.rgb, value, alpha), frames);
    outputFast[id.xy] = float4(lerp(fastHistory.rgb, value, fastAlpha), lerp(fastHistory.a, moment2, alpha));
}
