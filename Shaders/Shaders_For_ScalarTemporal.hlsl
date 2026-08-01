/*
Key differences between the simplified TAA and the original TAA:

1. No Gaussian reconstruction is applied to reconstruct the current-frame signal from neighboring samples.
2. No color clamping is performed to constrain the history color to the current neighborhood.
3. No Catmull-Rom reconstruction is used for high-quality subpixel sampling of the history buffer.
4. No sharpening pass is applied to compensate for the softness introduced by temporal accumulation.
*/

cbuffer ScalarTemporalConstants : register(b0)
{
    float4x4 currJitteredInvViewProj;
    float4x4 prevUnjitteredViewProj;

    float historyWeight;
    float depthThreshold;
    float normalThreshold;
    float motionSensitivity;

    float signalDifferenceSensitivity;
    float backgroundValue;
    float2 resolution;

    uint historyValid;
    uint useGeometryRejection;
    uint currentSignalTextureIdx;
    uint previousHistoryTextureIdx;

    uint depthTextureIdx;
    uint motionTextureIdx;
    uint normalTextureIdx;
    uint previousDepthTextureIdx;

    uint previousNormalTextureIdx;
    uint3 padding;
};

SamplerState sPoint : register(s0);
SamplerState sLinear : register(s1);

#include "FullscreenTriangle.hlsli"
#include "MaterialCommon.hlsli"

static const float BACKGROUND_DEPTH = 0.999999f;

struct VS_OUTPUT
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};

struct ClosestDepthSample
{
    float depth;
    float2 uv;
};

struct Reprojection
{
    float2 historyUV;
    float expectedPreviousDepth;
    bool valid;
};

VS_OUTPUT VSMain(uint vertexID : SV_VertexID)
{
    VS_OUTPUT output;
    output.uv = GetFullscreenTriangleTexCoord(vertexID);
    output.pos = GetFullscreenTrianglePosition(output.uv);
    return output;
}

bool IsUVInsideViewport(float2 uv)
{
    return all(uv >= 0.0f) && all(uv <= 1.0f);
}

int2 ClampPixel(int2 pixel, uint width, uint height)
{
    return clamp(pixel, int2(0, 0), int2((int)width - 1, (int)height - 1));
}

ClosestDepthSample FindClosestDepthSample(Texture2D depthTexture, float2 uv)
{
    uint width;
    uint height;
    depthTexture.GetDimensions(width, height);

    int2 centerPixel = ClampPixel(int2(uv * float2(width, height)), width, height);
    int2 closestPixel = centerPixel;
    float closestDepth = depthTexture.Load(int3(centerPixel, 0)).r;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            int2 samplePixel = ClampPixel(centerPixel + int2(x, y), width, height);
            float sampleDepth = depthTexture.Load(int3(samplePixel, 0)).r;
            if (sampleDepth < closestDepth)
            {
                closestDepth = sampleDepth;
                closestPixel = samplePixel;
            }
        }
    }

    ClosestDepthSample result;
    result.depth = closestDepth;
    result.uv = (float2(closestPixel) + 0.5f) / float2(width, height);
    return result;
}

Reprojection ReprojectClosestDepth(ClosestDepthSample closestDepth, float2 currentUV)
{
    Reprojection result;
    result.historyUV = currentUV;
    result.expectedPreviousDepth = 1.0f;
    result.valid = false;

    float2 currentNDC = float2(
        closestDepth.uv.x * 2.0f - 1.0f,
        1.0f - closestDepth.uv.y * 2.0f);
    float4 currentClip = float4(currentNDC, closestDepth.depth, 1.0f);
    float4 worldPositionH = mul(currentClip, currJitteredInvViewProj);

    if (abs(worldPositionH.w) < 1.0e-6f)
    {
        return result;
    }

    float3 worldPosition = worldPositionH.xyz / worldPositionH.w;
    float4 previousClip = mul(float4(worldPosition, 1.0f), prevUnjitteredViewProj);
    if (abs(previousClip.w) < 1.0e-6f)
    {
        return result;
    }

    float3 previousNDC = previousClip.xyz / previousClip.w;
    float2 closestHistoryUV = float2(
        previousNDC.x * 0.5f + 0.5f,
        0.5f - previousNDC.y * 0.5f);

    result.historyUV = currentUV - (closestDepth.uv - closestHistoryUV);
    result.expectedPreviousDepth = previousNDC.z;
    result.valid = true;
    return result;
}

float ComputeGeometryConfidence(
    Texture2D currentNormal,
    Texture2D previousDepth,
    Texture2D previousNormal,
    ClosestDepthSample closestDepth,
    Reprojection reprojection)
{
    float sampledPreviousDepth =
        previousDepth.SampleLevel(sPoint, reprojection.historyUV, 0).r;

    float depthTolerance =
        max(depthThreshold + 2.0f * fwidth(reprojection.expectedPreviousDepth), 1.0e-6f);
    float depthConfidence =
        1.0f - saturate(
            abs(sampledPreviousDepth - reprojection.expectedPreviousDepth) /
            depthTolerance);

    float3 currentWorldNormal = normalize(
        DecodeGBufferNormal(
            currentNormal.SampleLevel(sPoint, closestDepth.uv, 0).xyz));
    float3 previousWorldNormal = normalize(
        DecodeGBufferNormal(
            previousNormal.SampleLevel(sPoint, reprojection.historyUV, 0).xyz));

    float normalSimilarity = dot(currentWorldNormal, previousWorldNormal);
    float safeNormalThreshold = min(normalThreshold, 0.999f);
    float normalConfidence = smoothstep(
        safeNormalThreshold,
        1.0f,
        normalSimilarity);

    return depthConfidence * normalConfidence;
}

float PSMain(VS_OUTPUT input) : SV_TARGET
{
    Texture2D currentSignal = ResourceDescriptorHeap[currentSignalTextureIdx];
    Texture2D previousHistory = ResourceDescriptorHeap[previousHistoryTextureIdx];
    Texture2D depthTexture = ResourceDescriptorHeap[depthTextureIdx];

    float centerDepth = depthTexture.SampleLevel(sPoint, input.uv, 0).r;
    if (centerDepth >= BACKGROUND_DEPTH)
    {
        return saturate(backgroundValue);
    }

    ClosestDepthSample closestDepth = FindClosestDepthSample(depthTexture, input.uv);
    float currentValue =
        saturate(currentSignal.SampleLevel(sPoint, input.uv, 0).r);

    if (historyValid == 0)
    {
        return currentValue;
    }

    Reprojection reprojection = ReprojectClosestDepth(closestDepth, input.uv);
    if (!reprojection.valid)
    {
        return currentValue;
    }

    float2 motionUV = input.uv - reprojection.historyUV;
    if (motionTextureIdx != 0xffffffffu)
    {
        Texture2D motionTexture = ResourceDescriptorHeap[motionTextureIdx];
        motionUV = motionTexture.SampleLevel(sPoint, closestDepth.uv, 0).rg;
        reprojection.historyUV = input.uv - motionUV;
    }

    if (!IsUVInsideViewport(reprojection.historyUV))
    {
        return currentValue;
    }

    float history = saturate(
        previousHistory.SampleLevel(sLinear, reprojection.historyUV, 0).r);

    float geometryConfidence = 1.0f;
    if (useGeometryRejection != 0)
    {
        Texture2D currentNormal = ResourceDescriptorHeap[normalTextureIdx];
        Texture2D previousDepth = ResourceDescriptorHeap[previousDepthTextureIdx];
        Texture2D previousNormal = ResourceDescriptorHeap[previousNormalTextureIdx];
        geometryConfidence = ComputeGeometryConfidence(
            currentNormal,
            previousDepth,
            previousNormal,
            closestDepth,
            reprojection);
    }

    float motionPixels = length(motionUV * resolution);
    float motionConfidence =
        rcp(1.0f + motionPixels * max(motionSensitivity, 0.0f));
    float signalConfidence =
        1.0f - saturate(
            abs(history - currentValue) *
            max(signalDifferenceSensitivity, 0.0f));

    float finalHistoryWeight = saturate(historyWeight) *
        geometryConfidence *
        motionConfidence *
        signalConfidence;

    return saturate(lerp(
        currentValue,
        history,
        finalHistoryWeight));
}
