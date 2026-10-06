#include "SSGICommon.hlsli"
#include "MaterialCommon.hlsli"
#include "FullscreenTriangle.hlsli"

cbuffer Constants : register(b0)
{
    float4x4 View, InvProj;
    uint2 Size, HalfSize;
    uint DepthIndex, NormalIndex, ORMIndex, AlbedoIndex;
    uint GIIndex;
    float Intensity;
    uint2 Padding;
};

float4 VSMain(uint id : SV_VertexID) : SV_POSITION
{
    return GetFullscreenTrianglePosition(GetFullscreenTriangleTexCoord(id));
}

float4 PSMain(float4 position : SV_POSITION) : SV_Target0
{
    uint2 pixel = uint2(position.xy);
    Texture2D<float> depth = ResourceDescriptorHeap[DepthIndex];
    Texture2D<float4> normal = ResourceDescriptorHeap[NormalIndex];
    Texture2D<float4> orm = ResourceDescriptorHeap[ORMIndex];
    Texture2D<float4> albedoTexture = ResourceDescriptorHeap[AlbedoIndex];
    Texture2D<float4> gi = ResourceDescriptorHeap[GIIndex];
    float z = depth.Load(int3(pixel, 0));
    float4 material = orm.Load(int3(pixel, 0));
    if (z >= 1 || material.a < 0.5) return 0;

    float2 uv = (pixel + 0.5) / Size;
    float3 p = SSGIViewPosition(uv, z, InvProj);
    float3 n = normalize(mul(float4(DecodeGBufferNormal(normal.Load(int3(pixel, 0)).xyz), 0), View).xyz);
    float footprint = max(length(SSGIViewPosition(uv + float2(1.0 / Size.x, 0), z, InvProj) - p),
        length(SSGIViewPosition(uv + float2(0, 1.0 / Size.y), z, InvProj) - p));
    float tolerance = max(min(footprint * 0.5, p.z * 0.01), 0.001);
    float2 halfPixel = float2(pixel) * 0.5;
    int2 base = int2(floor(halfPixel));
    float2 fraction = frac(halfPixel);
    float3 sum = 0;
    float weightSum = 0;
    [loop] for (int i = 0; i < 4; ++i)
    {
        int2 offset = int2(i & 1, i >> 1), tap = base + offset;
        if (any(tap >= int2(HalfSize))) continue;
        int2 fullPixel = tap * 2;
        float tapZ = depth.Load(int3(fullPixel, 0));
        if (tapZ >= 1 || orm.Load(int3(fullPixel, 0)).a < 0.5) continue;

        float3 tapN = normalize(mul(float4(DecodeGBufferNormal(normal.Load(int3(fullPixel, 0)).xyz), 0), View).xyz);
        float agreement = saturate(dot(n, tapN));
        if (agreement < 0.8) continue;
        float3 q = SSGIViewPosition((fullPixel + 0.5) / Size, tapZ, InvProj);
        float3 delta = q - p;
        float planeError = max(abs(dot(delta, n)), abs(dot(delta, tapN))) / tolerance;

        // Preserve coplanar surfaces using plane distance rather than point distance.
        if (planeError >= 3) continue;
        float2 bilinear = lerp(1 - fraction, fraction, float2(offset));
        float weight = bilinear.x * bilinear.y * pow(agreement, 32) * exp2(-4 * planeError * planeError);

        sum += gi.Load(int3(tap, 0)).rgb * weight; // Spatial variance in alpha is never a lighting factor.
        weightSum += weight;
    }
    float3 indirect = weightSum > 1e-5 ? sum / weightSum : 0;

    float3 albedo = albedoTexture.Load(int3(pixel, 0)).rgb;
    float3 F = fresnelSchlickRoughness(saturate(dot(n, normalize(-p))),
        ComputeMaterialF0(albedo, material.b), ClampPerceptualRoughness(material.g));
    // Input already stores E/pi. Apply receiver diffuse response once; additive blend preserves SceneColor alpha.
    float3 result = indirect * albedo * ComputeDiffuseEnergy(F, material.b) * Intensity;

    return float4(result, 0);
}
