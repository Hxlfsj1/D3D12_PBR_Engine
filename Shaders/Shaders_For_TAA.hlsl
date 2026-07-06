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
    float2 velocityUV = uv;
    
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
                velocityUV = uv + offset;
            }
        }
    }

    float xNDC = (velocityUV.x * 2.0f - 1.0f) - jitterOffset.x;
    float yNDC = (1.0f - velocityUV.y * 2.0f) - jitterOffset.y;
    float4 clipSpacePos = float4(xNDC, yNDC, minDepth, 1.0f);
    
    float4 worldPosH = mul(clipSpacePos, invViewProj);
    float3 worldPos = worldPosH.xyz / worldPosH.w;

    // Re-calculate previous UV (compute UVs dynamically to save VRAM)
    float4 prevClipSpacePos = mul(float4(worldPos, 1.0f), prevViewProj);
    float2 prevNDC = prevClipSpacePos.xy / prevClipSpacePos.w;
    float2 historyUV = float2(prevNDC.x * 0.5f + 0.5f, 0.5f - prevNDC.y * 0.5f);

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

    float3 historyColor = tHistoryColor.SampleLevel(sLinear, historyUV, 0).rgb;
    float3 historyYCoCg = mul(RGB_2_YCoCg, historyColor);
    
    // Define the Color Clamping Range (often referred to as a Color Bounding Box)
    float3 clampedHistoryY = clamp(historyYCoCg, boxMin, boxMax);
    float3 clampedHistoryRGB = mul(YCoCg_2_RGB, clampedHistoryY);

    float3 finalColor = lerp(sharpenedCenter, clampedHistoryRGB, blendAlpha);

    return float4(finalColor, 1.0f);
}
