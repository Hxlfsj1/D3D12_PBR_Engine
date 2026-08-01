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

CurrentNeighborhood ReconstructTSRCurrentNeighborhood(
    Texture2D currentColorTexture,
    float2 outputUV,
    uint inputWidth,
    uint inputHeight)
{
    const float gaussianFalloff = 2.29f;
    int2 inputDimensions = int2(inputWidth, inputHeight);

    // Pixel coordinates use integer pixel centers. Camera jitter moves the
    // low-resolution samples away from the output pixel being reconstructed
    float2 targetSourcePosition = outputUV * float2(inputWidth, inputHeight) - 0.5f;
    int2 nearestSourcePixel = int2(floor(
        targetSourcePosition + currentJitterPixels + 0.5f));

    float3 yCoCgSum = 0.0f;
    float3 yCoCgSquareSum = 0.0f;
    float3 colorSum = 0.0f;
    float3 reconstructedColor = 0.0f;
    float reconstructionWeightSum = 0.0f;

    [unroll]
    for (int x = -1; x <= 1; ++x)
    {
        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            int2 sourcePixel = clamp(
                nearestSourcePixel + int2(x, y),
                int2(0, 0),
                inputDimensions - 1);
            float3 neighbor = currentColorTexture.Load(int3(sourcePixel, 0)).rgb;

            AccumulateNeighborhoodStatistics(
                neighbor,
                colorSum,
                yCoCgSum,
                yCoCgSquareSum);

            float2 unjitteredSourcePosition =
                float2(sourcePixel) - currentJitterPixels;
            float2 sampleDelta = unjitteredSourcePosition - targetSourcePosition;
            float reconstructionWeight = exp(
                -gaussianFalloff * dot(sampleDelta, sampleDelta));
            reconstructedColor += neighbor * reconstructionWeight;
            reconstructionWeightSum += reconstructionWeight;
        }
    }

    reconstructedColor /= max(reconstructionWeightSum, 1.0e-6f);
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

    uint inputWidth, inputHeight;
    tCurrentColor.GetDimensions(inputWidth, inputHeight);
    float2 inputTexelSize = 1.0f / float2(inputWidth, inputHeight);

    uint outputWidth, outputHeight;
    tHistoryColor.GetDimensions(outputWidth, outputHeight);
    float2 outputTexelSize = 1.0f / float2(outputWidth, outputHeight);

    float2 uv = input.uv;
    ClosestDepthSample closestDepth = FindClosestDepthSample(
        tDepth,
        uv,
        inputTexelSize);

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

    CurrentNeighborhood currentNeighborhood = ReconstructTSRCurrentNeighborhood(
        tCurrentColor,
        uv,
        inputWidth,
        inputHeight);
    float3 sharpenedCurrent = SharpenCurrentColor(currentNeighborhood);

    if (blendAlpha <= 0.0f || !IsUVInsideViewport(historyUV))
    {
        return float4(sharpenedCurrent, 1.0f);
    }

    float3 historyColor = SampleHistoryCatmullRom(
        tHistoryColor,
        historyUV,
        outputTexelSize).rgb;
    float3 clampedHistoryColor = ClampHistoryColor(historyColor, currentNeighborhood);
    float historyBlend = ComputeHistoryBlend(
        blendAlpha,
        hasMotionTexture,
        motionUV,
        outputWidth,
        outputHeight);

    float3 finalColor = lerp(sharpenedCurrent, clampedHistoryColor, historyBlend);
    return float4(finalColor, 1.0f);
}
