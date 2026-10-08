cbuffer HBAOConstants : register(b0)
{
    float4x4 projMat;
    float4x4 invProjMat;
    float4x4 viewMat;
    float radius;
    float bias;
    float intensity;
    float resolutionX;
    float resolutionY;
    uint temporalFrameIndex;
    uint quality;
    uint pad;
};

cbuffer BindlessIndices : register(b1)
{
    uint texIdx0;
    uint texIdx1;
    uint texIdx2;
    uint pad2;
};

SamplerState sPoint : register(s0);
SamplerState sLinear : register(s1);

#include "MathCommon.hlsli"
#include "MaterialCommon.hlsli"
#include "GeometryCommon.hlsli"
#include "BlueNoise.hlsli"

static const float HBAO_BACKGROUND_DEPTH = 0.999999f;

bool IsBackgroundDepth(float depth)
{
    return depth >= HBAO_BACKGROUND_DEPTH;
}

struct VS_OUTPUT
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};

#include "FullscreenTriangle.hlsli"

// Draw a bufferless fullscreen triangle (the same as Shaders_For_Deferred.hlsl)
VS_OUTPUT VSMain(uint vertexID : SV_VertexID)
{
    VS_OUTPUT output;
    output.uv = GetFullscreenTriangleTexCoord(vertexID);
    output.pos = GetFullscreenTrianglePosition(output.uv);
    return output;
}

float4 PSMain_HBAO(VS_OUTPUT input) : SV_TARGET
{
    int2 resolution = int2(resolutionX, resolutionY);
    int2 centerPixel = int2(input.pos.xy);
    float2 centerUV = (float2(centerPixel) + 0.5f) / float2(resolution);
    Texture2D tDepth = ResourceDescriptorHeap[texIdx0];
    float centerDepth = tDepth.Load(int3(centerPixel, 0)).r;
    if (IsBackgroundDepth(centerDepth))
    {
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    // Unpack normal data in G-buffer
    Texture2D tNormal = ResourceDescriptorHeap[texIdx1];
    float3 worldNormal = normalize(DecodeGBufferNormal(tNormal.Load(int3(centerPixel, 0)).xyz));
    float3 viewNormal = normalize(mul(worldNormal, (float3x3) viewMat));

    float3 P = ReconstructPosition(centerUV, centerDepth, invProjMat);
    Texture2DArray<uint2> blueNoise = ResourceDescriptorHeap[texIdx2];

    // Rotate the sampling pattern every frame so temporal accumulation can
    // average independent HBAO estimates instead of repeatedly blending the
    // same deterministic screen-space pattern.
    float2 temporalNoiseOffset = float2(0.754877666f, 0.569840296f) * (float)(temporalFrameIndex & 7u);
    float randomAngle = Rand(input.uv + temporalNoiseOffset) * 3.1415926f * 2.0f;

    int numDirs = quality == 1u ? 4 : quality == 2u ? 6 : quality == 3u ? 8 : 12;
    int numSteps = quality == 1u ? 4 : quality < 4u ? 6 : 8;
    // Constant spacing preserves quality 1's existing sampling rays.
    float angularStep = quality == 1u ? (2.0f * 3.1415926f / 4.0f) :
        quality == 2u ? (2.0f * 3.1415926f / 6.0f) :
        quality == 3u ? (2.0f * 3.1415926f / 8.0f) :
        (2.0f * 3.1415926f / 12.0f);

    float ao = 0.0f;

    // Keep the original UV estimate and limits. Quality changes sampling density,
    // not the search envelope; even at the limits it matches quality 1's range.
    float stepSizeUV = (radius / P.z) / 4.0f;
    stepSizeUV = clamp(stepSizeUV, 0.001f, 0.05f);
    float searchRadiusUV = stepSizeUV * 4.0f;

    for (int i = 0; i < numDirs; ++i)
    {
        float angle = randomAngle + (float) i * angularStep;
        float2 dir = float2(cos(angle), sin(angle));
        float distanceJitter = LoadSTBN2D(blueNoise, uint2(centerPixel),
            temporalFrameIndex, uint(i) + 1u).y;
        float pixelsPerUV = length(dir * float2(resolution));
        float nearRangeUV = min(2.0f / pixelsPerUV, searchRadiusUV);
        int2 previousSamplePixel = centerPixel;

        // Apply angle bias to prevent surface acne
        float maxAngle = bias;

        for (int j = 1; j <= numSteps; ++j)
        {
            float rayUV;
            if (quality == 1u)
            {
                // Preserve the current jitter-only baseline exactly.
                rayUV = stepSizeUV * (float(j) - distanceJitter);
            }
            else if (j <= 2)
            {
                // Move 1 and 2 pixels along the original UV ray, retaining its
                // aspect behavior rather than introducing a new projection.
                rayUV = min(float(j) / pixelsPerUV, searchRadiusUV);
            }
            else
            {
                if (searchRadiusUV <= nearRangeUV)
                    break;
                // Ordered jittered strata; extra density near the receiver.
                float t = (float(j - 2) - distanceJitter) / float(numSteps - 2);
                rayUV = nearRangeUV + (searchRadiusUV - nearRangeUV) * t * t;
            }
            float2 offsetUV = input.uv + dir * rayUV;

            if (offsetUV.x < 0.0f || offsetUV.x > 1.0f || offsetUV.y < 0.0f || offsetUV.y > 1.0f)
                continue;

            // Keep point-sampling's texel choice, but reconstruct on that
            // texel's ray instead of the original unsnapped ray. The mismatch
            // is especially visible on surfaces viewed at a grazing angle.
            int2 samplePixel = clamp(int2(floor(offsetUV * float2(resolution))),
                int2(0, 0), resolution - 1);
            if (all(samplePixel == centerPixel) ||
                (quality > 1u && all(samplePixel == previousSamplePixel)))
                continue;
            previousSamplePixel = samplePixel;
            float2 sampleUV = (float2(samplePixel) + 0.5f) / float2(resolution);
            float sampleDepth = tDepth.Load(int3(samplePixel, 0)).r;
            if (IsBackgroundDepth(sampleDepth))
                continue;

            float3 S = ReconstructPosition(sampleUV, sampleDepth, invProjMat);
            float3 V = S - P;
            float dist = length(V);

            // Check distance to prevent halo artifacts
            if (dist < radius)
            {
                float currentAngle = dot(normalize(V), viewNormal);
                if (currentAngle > maxAngle)
                {
                    // Apply linear falloff based on distance
                    float falloff = 1.0f - (dist / radius);
                    ao += (currentAngle - maxAngle) * falloff;
                    maxAngle = currentAngle;
                }
            }
        }
    }

    ao = 1.0f - saturate((ao / (float) numDirs) * intensity);
    return float4(ao, ao, ao, 1.0f);
}

/*
Bilateral filtering defines a valid blending scope:
neighboring pixels with large depth/normal deltas (discontinuities) get near-zero weights, minimizes their AO contribution,
ensuring we only average across geometrically similar areas and ignore completely distinct surfaces
*/
float4 PSMain_Blur(VS_OUTPUT input) : SV_TARGET
{
    Texture2D tRawHBAO = ResourceDescriptorHeap[texIdx0];
    Texture2D tDepth = ResourceDescriptorHeap[texIdx1];
    Texture2D tNormal = ResourceDescriptorHeap[texIdx2];

    float centerDepth = tDepth.SampleLevel(sPoint, input.uv, 0).r;
    if (IsBackgroundDepth(centerDepth))
    {
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    float3 centerNormal = normalize(DecodeGBufferNormal(tNormal.SampleLevel(sPoint, input.uv, 0).xyz));

    float result = 0.0f;
    float weightSum = 0.0f;
    // Calculate texel size based on screen resolution
    float2 texelSize = float2(1.0f / resolutionX, 1.0f / resolutionY);

    for (int x = -2; x <= 2; ++x)
    {
        for (int y = -2; y <= 2; ++y)
        {
            // Fetch sample data
            float2 offset = float2((float) x, (float) y) * texelSize;
            float2 sampleUV = input.uv + offset;
            if (sampleUV.x < 0.0f || sampleUV.x > 1.0f || sampleUV.y < 0.0f || sampleUV.y > 1.0f)
                continue;

            float sampleDepth = tDepth.SampleLevel(sPoint, sampleUV, 0).r;
            if (IsBackgroundDepth(sampleDepth))
                continue;

            float sampleAO = tRawHBAO.SampleLevel(sLinear, sampleUV, 0).r;
            float3 sampleNormal = normalize(DecodeGBufferNormal(tNormal.SampleLevel(sPoint, sampleUV, 0).xyz));

            // Spatial weight (Distance falloff)
            float spatialWeight = exp(-(x * x + y * y) / 4.0f);
            // Depth weight (Edge preservation)
            float depthWeight = exp(-abs(centerDepth - sampleDepth) * 100.0f);
            // Normal weight (Angle-based rejection)
            float normalWeight = pow(max(dot(centerNormal, sampleNormal), 0.0f), 16.0f);

            // Combine weights to compute the final bilateral weight
            float weight = spatialWeight * depthWeight * normalWeight;

            result += sampleAO * weight;
            weightSum += weight;
        }
    }

    // Use a weighted average since each pixel's contribution varies
    result /= max(weightSum, 0.0001f);
    return float4(result, result, result, 1.0f);
}
