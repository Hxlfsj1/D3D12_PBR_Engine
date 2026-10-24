#include "FullscreenTriangle.hlsli"

cbuffer PreviewConstants : register(b0)
{
    uint sourceIndex, beforeIndex, mapping, channel;
    uint slice, arraySize, sourceWidth, sourceHeight;
    float nearZ, farZ, exposure, motionScale;
};

struct VertexOutput { float4 position : SV_Position; float2 uv : TEXCOORD; };
VertexOutput VSMain(uint vertex : SV_VertexID)
{
    VertexOutput result;
    result.uv = GetFullscreenTriangleTexCoord(vertex);
    result.position = GetFullscreenTrianglePosition(result.uv);
    return result;
}

float4 LoadValue(uint descriptor, float2 uv, uint layer)
{
    uint2 pixel = min(uint2(saturate(uv) * uint2(sourceWidth, sourceHeight)),
                      uint2(sourceWidth, sourceHeight) - 1);
    if (arraySize > 1)
    {
        Texture2DArray<float4> texture = ResourceDescriptorHeap[descriptor];
        return texture.Load(int4(pixel, min(layer, arraySize - 1), 0));
    }
    Texture2D<float4> texture = ResourceDescriptorHeap[descriptor];
    return texture.Load(int3(pixel, 0));
}

float3 DisplayHDR(float3 color)
{
    color = max(color * exposure, 0);
    color = saturate((color * (2.51 * color + 0.03)) /
                     (color * (2.43 * color + 0.59) + 0.14));
    return pow(color, 1.0 / 2.2);
}

float4 PSMain(VertexOutput input) : SV_Target0
{
    float2 uv = input.uv;
    uint layer = slice == 0xffffffff ? 0 : slice;
    if (mapping == 6 && arraySize > 1 && slice == 0xffffffff)
    {
        uint2 tile = min(uint2(saturate(uv) * 2), 1u);
        layer = tile.x + tile.y * 2;
        uv = frac(uv * 2);
    }
    float4 value = LoadValue(sourceIndex, uv, layer);
    if (mapping == 8) return value - LoadValue(beforeIndex, uv, layer);
    if (!all(isfinite(value))) return float4(1, 0, 1, 1);
    if (mapping == 0) return float4(DisplayHDR(value.rgb), 1);
    if (mapping == 1) return float4(saturate(value.rgb), 1);
    if (mapping == 2) return float4(saturate(value.xyz), 1); // GBuffer stores encoded normals.
    if (mapping == 3) return float4(saturate(value[channel]).xxx, 1);
    if (mapping == 4)
    {
        float depth = nearZ * farZ / max(farZ - value.x * (farZ - nearZ), 1e-6);
        float display = value.x < 1 ? log2(1 + depth) / log2(1 + farZ) : 0;
        return float4(saturate(display).xxx, 1);
    }
    if (mapping == 5)
    {
        if (any(abs(value.xy) > 1)) return float4(1, 0, 1, 1);
        return float4(saturate(0.5 + value.xy * motionScale), 0.5, 1);
    }
    if (mapping == 6) return float4(saturate(value.x).xxx, 1);
    if (mapping == 9) return float4(pow(saturate(value.rgb), 1.0 / 2.2), 1);
    // Signed linear change: orange adds light; blue removes light. Sum channels
    // separately so colored brightening/darkening are not cancelled to zero.
    float3 delta = value.rgb;
    float positive = dot(max(delta, 0), float3(0.2126, 0.7152, 0.0722));
    float negative = dot(max(-delta, 0), float3(0.2126, 0.7152, 0.0722));
    float3 heat = positive * float3(1, 0.45, 0.05) + negative * float3(0.05, 0.45, 1);
    return float4(DisplayHDR(heat), 1);
}
