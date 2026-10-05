#include "SSGICommon.hlsli"

cbuffer Constants : register(b0)
{
    float4x4 View, InvProj;
    uint2 Size, OutputSize;
    uint DepthIndex, NormalIndex, ORMIndex, RawIndex;
    uint OutputIndex;
    uint3 Padding;
};

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= OutputSize)) return;
    Texture2D<float> depth = ResourceDescriptorHeap[DepthIndex];
    Texture2D<float4> normals = ResourceDescriptorHeap[NormalIndex];
    Texture2D<float4> orm = ResourceDescriptorHeap[ORMIndex];
    Texture2D<float4> raw = ResourceDescriptorHeap[RawIndex];
    RWTexture2D<float4> output = ResourceDescriptorHeap[OutputIndex];
    uint2 pixel = id.xy * 2;
    float z = depth.Load(int3(pixel, 0));
    output[id.xy] = 0;
    if (z >= 1 || orm.Load(int3(pixel, 0)).a < 0.5) return;

    float2 uv = (pixel + 0.5) / Size;
    float3 p = SSGIViewPosition(uv, z, InvProj);
    float3 n = normalize(mul(float4(normals.Load(int3(pixel, 0)).xyz * 2 - 1, 0), View).xyz);
    float footprintX = length(SSGIViewPosition(uv + float2(1.0 / Size.x, 0), z, InvProj) - p);
    float footprintY = length(SSGIViewPosition(uv + float2(0, 1.0 / Size.y), z, InvProj) - p);
    float planeTolerance = max(0.5 * max(footprintX, footprintY), 1e-4);

    // Binomial 5x5 kernel [1,4,6,4,1] in each axis. Gather more current-frame
    // samples without extending temporal persistence. Geometry weights preserve surface edges.
    // Keep the center unconditional for isolated surfaces/1x1 views.
    float4 sum = raw.Load(int3(id.xy, 0)) * 36;
    float weightSum = 36;
    [loop] for (int y = -2; y <= 2; ++y)
    [loop] for (int x = -2; x <= 2; ++x)
    {
        if (x == 0 && y == 0) continue;
        int2 tap = int2(id.xy) + int2(x, y);
        if (any(tap < 0) || any(tap >= int2(OutputSize))) continue;
        int2 fullPixel = tap * 2;
        float tapZ = depth.Load(int3(fullPixel, 0));
        if (tapZ >= 1 || orm.Load(int3(fullPixel, 0)).a < 0.5) continue;
        float3 tapN = normalize(mul(float4(normals.Load(int3(fullPixel, 0)).xyz * 2 - 1, 0), View).xyz);

        float normalAgreement = saturate(dot(n, tapN));
        if (normalAgreement < 0.8) continue;
        float3 tapP = SSGIViewPosition((fullPixel + 0.5) / Size, tapZ, InvProj);
        float3 delta = tapP - p;
        // Symmetric tangent-plane distance preserves sloped surfaces better than |z0-z1|.
        float planeError = max(abs(dot(delta, n)), abs(dot(delta, tapN))) / planeTolerance;
        if (planeError >= 3) continue;
        float spatialWeight = (x == 0 ? 6 : (abs(x) == 1 ? 4 : 1)) *
            (y == 0 ? 6 : (abs(y) == 1 ? 4 : 1));
        float weight = spatialWeight * pow(normalAgreement, 32) * exp2(-4 * planeError * planeError);
        sum += raw.Load(int3(tap, 0)) * weight;
        weightSum += weight;
    }
    // RGB remains E/pi; alpha is the filtered hit fraction.
    // Zero-radiance misses are valid samples, not holes. Never divide by hit fraction.
    output[id.xy] = sum / weightSum;

}
