cbuffer TAAConstants : register(b0)
{
    float4x4 invViewProj;
    float4x4 prevViewProj;
    float2 jitterOffset;
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

float4 PSMain(VS_OUTPUT input) : SV_TARGET
{
    Texture2D tCurrentColor = ResourceDescriptorHeap[colorTextureIdx];
    Texture2D tHistoryColor = ResourceDescriptorHeap[historyTextureIdx];
    Texture2D tDepth = ResourceDescriptorHeap[depthTextureIdx];

    float2 uv = input.uv;

    uint width, height;
    tCurrentColor.GetDimensions(width, height);
    float2 texelSize = float2(1.0f / width, 1.0f / height);

    float3 centerColor = tCurrentColor.SampleLevel(sPoint, uv, 0).rgb;
    float depth = tDepth.SampleLevel(sPoint, uv, 0).r;

    float minDepth = depth;
    float2 closestDepthUV = uv;
    
    // 3x3 depth dilation (fully unrolled to avoid branch overhead)
    [unroll]
    for (int i = -1; i <= 1; ++i)
    {
        [unroll]
        for (int j = -1; j <= 1; ++j)
        {
            float2 offset = float2(i, j) * texelSize;
            float d = tDepth.SampleLevel(sPoint, uv + offset, 0).r;
            if (d < minDepth)
            {
                minDepth = d;
                closestDepthUV = uv + offset;
            }
        }
    }

    float2 motionUV = 0.0f;
    float2 historyUV = closestDepthUV;
    bool hasMotionTexture = motionTextureIdx != 0xffffffffu;
    if (hasMotionTexture)
    {
        Texture2D tMotionUV = ResourceDescriptorHeap[motionTextureIdx];
        motionUV = tMotionUV.SampleLevel(sPoint, closestDepthUV, 0).rg;
        historyUV = closestDepthUV - motionUV;
    }
    else
    {
        float xNDC = closestDepthUV.x * 2.0f - 1.0f;
        float yNDC = 1.0f - closestDepthUV.y * 2.0f;
        float4 clipSpacePos = float4(xNDC, yNDC, minDepth, 1.0f);

        float4 worldPosH = mul(clipSpacePos, invViewProj);
        float3 worldPos = worldPosH.xyz / worldPosH.w;

        float4 prevClipSpacePos = mul(float4(worldPos, 1.0f), prevViewProj);
        float2 prevNDC = prevClipSpacePos.xy / prevClipSpacePos.w;
        historyUV = float2(prevNDC.x * 0.5f + 0.5f, 0.5f - prevNDC.y * 0.5f);
    }

    // m1 stores YCoCg sum, m2 stores YCoCg sum of squares (Var(X) = E(X²) - E(X)²)
    float3 m1 = 0.0f;
    float3 m2 = 0.0f;
    float3 blurredCenter = 0.0f;

    [unroll]
    for (int x = -1; x <= 1; ++x)
    {
        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            float3 neighbor = tCurrentColor.SampleLevel(sPoint, uv + float2(x, y) * texelSize, 0).rgb;
            float3 neighborY = mul(RGB_2_YCoCg, neighbor);
            m1 += neighborY;
            m2 += neighborY * neighborY;
            
            blurredCenter += neighbor;
        }
    }
    
    float3 mu = m1 / 9.0f;
    // Standard deviation
    float3 sigma = sqrt(abs(m2 / 9.0f - mu * mu));
    
    // Calculate color bounding box
    float3 boxMin = mu - 1.25f * sigma;
    float3 boxMax = mu + 1.25f * sigma;

    // Laplacian sharpening
    blurredCenter /= 9.0f;
    float sharpenAmount = 0.25f;
    float3 sharpenedCenter = centerColor + (centerColor - blurredCenter) * sharpenAmount;
    sharpenedCenter = max(0.0f, sharpenedCenter);

    // Off-screen history rejection: do not blend new pixels entering from outside the screen
    if (historyUV.x < 0.0f || historyUV.x > 1.0f || historyUV.y < 0.0f || historyUV.y > 1.0f)
    {
        return float4(sharpenedCenter, 1.0f);
    }

    float3 historyColor = SampleHistoryCatmullRom(tHistoryColor, sPoint, historyUV, texelSize).rgb;
    float3 historyYCoCg = mul(RGB_2_YCoCg, historyColor);
    
    // Define the Color Clamping Range (often referred to as a Color Bounding Box)
    float3 clampedHistoryY = clamp(historyYCoCg, boxMin, boxMax);
    float3 clampedHistoryRGB = mul(YCoCg_2_RGB, clampedHistoryY);

    float historyBlend = blendAlpha;
    if (hasMotionTexture && blendAlpha > 0.0f)
    {
        float motionPixels = length(motionUV * float2(width, height));
        float currentWeight = lerp(0.05f, 0.20f, saturate(motionPixels / 40.0f));
        historyBlend = 1.0f - currentWeight;
    }

    float3 finalColor = lerp(sharpenedCenter, clampedHistoryRGB, historyBlend);

    return float4(finalColor, 1.0f);
}
