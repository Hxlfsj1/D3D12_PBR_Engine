SamplerState sPoint : register(s0);
SamplerState sLinear : register(s1);

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

float4 SampleHistoryCatmullRom(
    Texture2D historyTexture,
    float2 uv,
    float2 texelSize)
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
            result += historyTexture.SampleLevel(sPoint, sampleUV, 0) * (weightX * weightY);
        }
    }

    return result;
}

struct ClosestDepthSample
{
    float depth;
    float2 uv;
};

struct CurrentNeighborhood
{
    float3 reconstructedColor;
    float3 averageColor;
    float3 meanYCoCg;
    float3 sigmaYCoCg;
};

void AccumulateNeighborhoodStatistics(
    float3 color,
    inout float3 colorSum,
    inout float3 yCoCgSum,
    inout float3 yCoCgSquareSum)
{
    float3 colorYCoCg = mul(RGB_2_YCoCg, color);
    colorSum += color;
    yCoCgSum += colorYCoCg;
    yCoCgSquareSum += colorYCoCg * colorYCoCg;
}

CurrentNeighborhood FinalizeCurrentNeighborhood(
    float3 reconstructedColor,
    float3 colorSum,
    float3 yCoCgSum,
    float3 yCoCgSquareSum,
    float sampleCount)
{
    CurrentNeighborhood neighborhood;
    neighborhood.reconstructedColor = reconstructedColor;
    neighborhood.averageColor = colorSum / sampleCount;
    neighborhood.meanYCoCg = yCoCgSum / sampleCount;
    neighborhood.sigmaYCoCg = sqrt(abs(
        yCoCgSquareSum / sampleCount -
        neighborhood.meanYCoCg * neighborhood.meanYCoCg));
    return neighborhood;
}

ClosestDepthSample FindClosestDepthSample(
    Texture2D depthTexture,
    float2 uv,
    float2 texelSize)
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

float2 ReprojectHistoryUV(
    float2 currentUV,
    ClosestDepthSample closestDepth,
    float4x4 currentInvViewProj,
    float4x4 previousViewProj)
{
    float2 currentNDC = float2(
        closestDepth.uv.x * 2.0f - 1.0f,
        1.0f - closestDepth.uv.y * 2.0f);
    float4 currentClipPos = float4(currentNDC, closestDepth.depth, 1.0f);

    float4 worldPosH = mul(currentClipPos, currentInvViewProj);
    float3 worldPos = worldPosH.xyz / worldPosH.w;

    float4 previousClipPos = mul(float4(worldPos, 1.0f), previousViewProj);
    float2 previousNDC = previousClipPos.xy / previousClipPos.w;
    float2 closestHistoryUV = float2(
        previousNDC.x * 0.5f + 0.5f,
        0.5f - previousNDC.y * 0.5f);

    return currentUV - (closestDepth.uv - closestHistoryUV);
}

float3 SharpenCurrentColor(CurrentNeighborhood neighborhood)
{
    const float sharpenAmount = 0.25f;
    float3 sharpenedColor = neighborhood.reconstructedColor +
        (neighborhood.reconstructedColor - neighborhood.averageColor) * sharpenAmount;
    return max(0.0f, sharpenedColor);
}

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

float ComputeHistoryBlend(
    float baseHistoryBlend,
    bool hasMotionTexture,
    float2 motionUV,
    uint width,
    uint height)
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
