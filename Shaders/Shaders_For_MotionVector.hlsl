cbuffer MotionVectorConstants : register(b0)
{
    float4x4 currJitteredInvViewProj;
    float4x4 prevUnjitteredViewProj;
    uint depthTextureIdx;
};

SamplerState sPoint : register(s0);

struct VS_OUTPUT
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};

#include "FullscreenTriangle.hlsli"

VS_OUTPUT VSMain(uint vertexID : SV_VertexID)
{
    VS_OUTPUT output;
    output.uv = GetFullscreenTriangleTexCoord(vertexID);
    output.pos = GetFullscreenTrianglePosition(output.uv);
    return output;
}

float2 PSMain(VS_OUTPUT input) : SV_TARGET
{
    Texture2D<float> tDepth = ResourceDescriptorHeap[depthTextureIdx];

    float2 uv = input.uv;
    float depth = tDepth.SampleLevel(sPoint, uv, 0).r;
    float2 currentNDC = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

    float4 worldPosH = mul(float4(currentNDC, depth, 1.0f), currJitteredInvViewProj);
    if (abs(worldPosH.w) < 1.0e-6f)
    {
        return 0.0f;
    }

    float3 worldPos = worldPosH.xyz / worldPosH.w;
    float4 prevClipPos = mul(float4(worldPos, 1.0f), prevUnjitteredViewProj);
    if (abs(prevClipPos.w) < 1.0e-6f)
    {
        return float2(2.0f, 2.0f);
    }

    float2 prevNDC = prevClipPos.xy / prevClipPos.w;
    float2 historyUV = float2(prevNDC.x * 0.5f + 0.5f, 0.5f - prevNDC.y * 0.5f);

    return uv - historyUV;
}
