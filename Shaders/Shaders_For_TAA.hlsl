cbuffer TAAConstants : register(b0)
{
    float4x4 currJitteredInvViewProj;
    float4x4 prevUnjitteredViewProj;
    float4 currentReconstructionWeights[3];

    float blendAlpha;
    uint colorTextureIdx;
    uint historyTextureIdx;
    uint depthTextureIdx;

    uint motionTextureIdx;
    float2 currentJitterPixels;
    uint constantsPadding;
};

struct VS_OUTPUT
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};

#include "FullscreenTriangle.hlsli"
#include "TemporalAACommon.hlsli"

VS_OUTPUT VSMain(uint vertexID : SV_VertexID)
{
    VS_OUTPUT output;
    output.uv = GetFullscreenTriangleTexCoord(vertexID);
    output.pos = GetFullscreenTrianglePosition(output.uv);
    return output;
}

CurrentNeighborhood AnalyzeTAACurrentNeighborhood(
    Texture2D currentColorTexture,
    float2 uv,
    float2 texelSize)
{
    float3 yCoCgSum = 0.0f;
    float3 yCoCgSquareSum = 0.0f;
    float3 colorSum = 0.0f;
    float3 reconstructedColor = 0.0f;

    [unroll]
    for (int x = -1; x <= 1; ++x)
    {
        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            float2 sampleOffset = float2(x, y);
            float3 neighbor = currentColorTexture.SampleLevel(
                sPoint,
                uv + sampleOffset * texelSize,
                0).rgb;

            AccumulateNeighborhoodStatistics(
                neighbor,
                colorSum,
                yCoCgSum,
                yCoCgSquareSum);

            float reconstructionWeight = currentReconstructionWeights[x + 1][y + 1];
            reconstructedColor += neighbor * reconstructionWeight;
        }
    }

    return FinalizeCurrentNeighborhood(
        reconstructedColor,
        colorSum,
        yCoCgSum,
        yCoCgSquareSum,
        9.0f);
}

float4 PSMain(VS_OUTPUT input) : SV_TARGET
{
    Texture2D tCurrentColor = ResourceDescriptorHeap[colorTextureIdx];
    Texture2D tHistoryColor = ResourceDescriptorHeap[historyTextureIdx];
    Texture2D tDepth = ResourceDescriptorHeap[depthTextureIdx];

    float2 uv = input.uv;

    uint width, height;
    tCurrentColor.GetDimensions(width, height);
    float2 texelSize = 1.0f / float2(width, height);

    ClosestDepthSample closestDepth = FindClosestDepthSample(tDepth, uv, texelSize);

    float2 motionUV = 0.0f;
    float2 historyUV;
    bool hasMotionTexture = motionTextureIdx != 0xffffffffu;
    if (hasMotionTexture)
    {
        Texture2D tMotionUV = ResourceDescriptorHeap[motionTextureIdx];
        motionUV = tMotionUV.SampleLevel(sPoint, closestDepth.uv, 0).rg;
        historyUV = uv - motionUV;
    }
    else
    {
        historyUV = ReprojectHistoryUV(
            uv,
            closestDepth,
            currJitteredInvViewProj,
            prevUnjitteredViewProj);
    }

    CurrentNeighborhood currentNeighborhood = AnalyzeTAACurrentNeighborhood(
        tCurrentColor,
        uv,
        texelSize);
    float3 sharpenedCurrent = SharpenCurrentColor(currentNeighborhood);

    if (blendAlpha <= 0.0f || !IsUVInsideViewport(historyUV))
    {
        return float4(sharpenedCurrent, 1.0f);
    }

    float3 historyColor = SampleHistoryCatmullRom(
        tHistoryColor,
        historyUV,
        texelSize).rgb;
    float3 clampedHistoryColor = ClampHistoryColor(historyColor, currentNeighborhood);
    float historyBlend = ComputeHistoryBlend(
        blendAlpha,
        hasMotionTexture,
        motionUV,
        width,
        height);

    float3 finalColor = lerp(sharpenedCurrent, clampedHistoryColor, historyBlend);
    return float4(finalColor, 1.0f);
}
