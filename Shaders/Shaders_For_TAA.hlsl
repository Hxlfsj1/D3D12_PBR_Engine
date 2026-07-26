cbuffer TAAConstants : register(b0)
{
    float4x4 currJitteredInvViewProj;
    float4x4 prevUnjitteredViewProj;
    float4 currentReconstructionWeights[3];

    float blendAlpha;
    float varianceScale;

    uint colorTextureIdx;
    uint historyTextureIdx;
    uint depthTextureIdx;
    uint motionTextureIdx;
};

SamplerState sPoint : register(s0);
SamplerState sLinear : register(s1);

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

// YCoCg separates luma from chroma for neighborhood statistics and history clamping
static const float3x3 RGB_2_YCoCg = float3x3
(
    0.25, 0.5, 0.25,
    0.5, 0.0, -0.5,
   -0.25, 0.5, -0.25
);

static const float3x3 YCoCg_2_RGB = float3x3
(
    1.0, 1.0, -1.0,
    1.0, 0.0, 1.0,
    1.0, -1.0, -1.0
);

// Catmull–Rom interpolation uses small negative weights on the outer samples to counteract blurring
float CatmullRomWeight(float x)
{
    x = abs(x);
    float x2 = x * x;
    float x3 = x2 * x;

    if (x <= 1.0f)
    {
        return 1.5f * x3 - 2.5f * x2 + 1.0f;
    }

    if (x < 2.0f)
    {
        return -0.5f * x3 + 2.5f * x2 - 4.0f * x + 2.0f;
    }

    return 0.0f;
}

float4 SampleHistoryCatmullRom(Texture2D historyTexture, SamplerState pointSampler, float2 uv, float2 texelSize)
{
    float2 textureSize = 1.0f / texelSize;
    float2 samplePos = uv * textureSize - 0.5f;
    float2 basePos = floor(samplePos);
    float2 fraction = samplePos - basePos;
    float2 minUV = texelSize * 0.5f;
    float2 maxUV = 1.0f - minUV;

    float4 result = 0.0f;

    [unroll]
    for (int y = -1; y <= 2; ++y)
    {
        float weightY = CatmullRomWeight((float)y - fraction.y);

        [unroll]
        for (int x = -1; x <= 2; ++x)
        {
            float weightX = CatmullRomWeight((float)x - fraction.x);
            float2 sampleUV = (basePos + float2(x, y) + 0.5f) * texelSize;
            sampleUV = clamp(sampleUV, minUV, maxUV);

            result += historyTexture.SampleLevel(pointSampler, sampleUV, 0) * (weightX * weightY);
        }
    }

    return result;
}

struct ClosestDepthSample
{
    float depth;
    float2 uv;
};

/*
Result of 3*3 analization :

reconstructedColor : Current-frame pixel color used for blending
averageColor : The value used to sharpen the current color
meanYCoCg : Center of the valid range for the history color
sigmaYCoCg : Extent of the valid range for the history color
*/
struct CurrentNeighborhood
{
    float3 reconstructedColor;
    float3 averageColor;
    float3 meanYCoCg;
    float3 sigmaYCoCg;
};

// Used when sampling MotionUV to provide dilation and prevent background motion vectors from being sampled
ClosestDepthSample FindClosestDepthSample(Texture2D depthTexture, float2 uv, float2 texelSize)
{
    ClosestDepthSample closest;
    closest.depth = depthTexture.SampleLevel(sPoint, uv, 0).r;
    closest.uv = uv;

    [unroll]
    for (int x = -1; x <= 1; ++x)
    {
        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            float2 sampleUV = uv + float2(x, y) * texelSize;
            float sampleDepth = depthTexture.SampleLevel(sPoint, sampleUV, 0).r;
            if (sampleDepth < closest.depth)
            {
                closest.depth = sampleDepth;
                closest.uv = sampleUV;
            }
        }
    }

    return closest;
}

// Matrix reprojection fallback used when MotionUV is unavailable, but the reprojection logic is same as MotionUV
float2 ReprojectHistoryUV(float2 currentUV, ClosestDepthSample closestDepth, float4x4 currentInvViewProj, float4x4 previousViewProj)
{
    float2 currentNDC = float2(closestDepth.uv.x * 2.0f - 1.0f, 1.0f - closestDepth.uv.y * 2.0f);
    float4 currentClipPos = float4(currentNDC, closestDepth.depth, 1.0f);

    float4 worldPosH = mul(currentClipPos, currentInvViewProj);
    float3 worldPos = worldPosH.xyz / worldPosH.w;

    float4 previousClipPos = mul(float4(worldPos, 1.0f), previousViewProj);
    float2 previousNDC = previousClipPos.xy / previousClipPos.w;
    float2 closestHistoryUV = float2(previousNDC.x * 0.5f + 0.5f, 0.5f - previousNDC.y * 0.5f);

    return currentUV - (closestDepth.uv - closestHistoryUV);
}

CurrentNeighborhood AnalyzeCurrentNeighborhood(Texture2D currentColorTexture, float2 uv, float2 texelSize)
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
            float3 neighbor = currentColorTexture.SampleLevel(sPoint, uv + sampleOffset * texelSize, 0).rgb;
            float3 neighborYCoCg = mul(RGB_2_YCoCg, neighbor);

            yCoCgSum += neighborYCoCg;
            yCoCgSquareSum += neighborYCoCg * neighborYCoCg;
            colorSum += neighbor;

            float reconstructionWeight = currentReconstructionWeights[x + 1][y + 1];
            reconstructedColor += neighbor * reconstructionWeight;
        }
    }

    CurrentNeighborhood neighborhood;
    neighborhood.reconstructedColor = reconstructedColor;
    neighborhood.averageColor = colorSum / 9.0f;
    neighborhood.meanYCoCg = yCoCgSum / 9.0f;
    neighborhood.sigmaYCoCg = sqrt(abs(yCoCgSquareSum / 9.0f - neighborhood.meanYCoCg * neighborhood.meanYCoCg));
    return neighborhood;
}

float3 SharpenCurrentColor(CurrentNeighborhood neighborhood)
{
    const float sharpenAmount = 0.25f;
    float3 sharpenedColor = neighborhood.reconstructedColor + (neighborhood.reconstructedColor - neighborhood.averageColor) * sharpenAmount;
    return max(0.0f, sharpenedColor);
}

// Clamp history color just like limit it in an AABB box
float3 ClampHistoryColor(float3 historyColor, CurrentNeighborhood neighborhood)
{
    float3 boxMin = neighborhood.meanYCoCg - 1.25f * neighborhood.sigmaYCoCg;
    float3 boxMax = neighborhood.meanYCoCg + 1.25f * neighborhood.sigmaYCoCg;
    float3 historyYCoCg = mul(RGB_2_YCoCg, historyColor);
    float3 clampedHistoryYCoCg = clamp(historyYCoCg, boxMin, boxMax);
    return mul(YCoCg_2_RGB, clampedHistoryYCoCg);
}

bool IsUVInsideViewport(float2 uv)
{
    return uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f;
}

float ComputeHistoryBlend(float baseHistoryBlend, bool hasMotionTexture, float2 motionUV, uint width, uint height)
{
    if (!hasMotionTexture || baseHistoryBlend <= 0.0f)
    {
        return baseHistoryBlend;
    }

    float motionPixels = length(motionUV * float2(width, height));
    float motionFactor = saturate(motionPixels / 40.0f);
    float movingHistoryBlend = min(baseHistoryBlend, 0.80f);
    return lerp(baseHistoryBlend, movingHistoryBlend, motionFactor);
}

float4 PSMain(VS_OUTPUT input) : SV_TARGET
{
    Texture2D tCurrentColor = ResourceDescriptorHeap[colorTextureIdx];
    Texture2D tHistoryColor = ResourceDescriptorHeap[historyTextureIdx];
    Texture2D tDepth = ResourceDescriptorHeap[depthTextureIdx];

    float2 uv = input.uv;

    uint width, height;
    tCurrentColor.GetDimensions(width, height);
    float2 texelSize = float2(1.0f / width, 1.0f / height);

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
        historyUV = ReprojectHistoryUV(uv, closestDepth, currJitteredInvViewProj, prevUnjitteredViewProj);
    }

    CurrentNeighborhood currentNeighborhood = AnalyzeCurrentNeighborhood(tCurrentColor, uv, texelSize);
    float3 sharpenedCurrent = SharpenCurrentColor(currentNeighborhood);

    if (blendAlpha <= 0.0f)
    {
        return float4(sharpenedCurrent, 1.0f);
    }

    if (!IsUVInsideViewport(historyUV))
    {
        return float4(sharpenedCurrent, 1.0f);
    }

    float3 historyColor = SampleHistoryCatmullRom(tHistoryColor, sPoint, historyUV, texelSize).rgb;
    float3 clampedHistoryColor = ClampHistoryColor(historyColor, currentNeighborhood);
    float historyBlend = ComputeHistoryBlend(blendAlpha, hasMotionTexture, motionUV, width, height);

    float3 finalColor = lerp(sharpenedCurrent, clampedHistoryColor, historyBlend);
    return float4(finalColor, 1.0f);
}
