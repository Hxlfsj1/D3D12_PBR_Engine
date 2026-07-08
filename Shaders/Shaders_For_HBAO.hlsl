cbuffer HBAOConstants : register(b0)
{
    float4x4 projMat;
    float4x4 invProjMat;
    float4x4 viewMat;
    float radius;
    float bias;
    float power;
    float resolutionX;
    float resolutionY;
    uint3 pad;
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

/*
Reconstruct view space position from NDC coordinates and depth

We compute in View Space for two reasons: First,
it's completely sufficient since AO doesn't rely on world-space lights.
Second, it saves 16 expensive inverse view matrix multiplications per pixel during ray marching.
*/
float3 GetViewPos(float2 uv, uint depthIdx)
{
    Texture2D tDepth = ResourceDescriptorHeap[depthIdx];
    float depth = tDepth.SampleLevel(sPoint, uv, 0).r;
    
    float x = uv.x * 2.0f - 1.0f;
    float y = (1.0f - uv.y) * 2.0f - 1.0f;
    float4 clipSpace = float4(x, y, depth, 1.0f);
    float4 viewSpace = mul(clipSpace, invProjMat);
    
    return viewSpace.xyz / viewSpace.w;
}

float4 PSMain_HBAO(VS_OUTPUT input) : SV_TARGET
{
    // Unpack normal data in G-buffer
    Texture2D tNormal = ResourceDescriptorHeap[texIdx1];
    float3 worldNormal = normalize(DecodeGBufferNormal(tNormal.SampleLevel(sPoint, input.uv, 0).xyz));
    float3 viewNormal = normalize(mul(worldNormal, (float3x3) viewMat));

    float3 P = GetViewPos(input.uv, texIdx0);
    
    // Apply a random rotation offset to the 4 sampling directions
    float randomAngle = Rand(input.uv) * 3.1415926f * 2.0f;
    
    // Define ray marching parameters
    int numDirs = 4;
    int numSteps = 4;
    
    float ao = 0.0f;
    
    // Calculate and clamp the UV step size
    float stepSizeUV = (radius / P.z) / (float) numSteps;
    stepSizeUV = clamp(stepSizeUV, 0.001f, 0.05f);
    
    for (int i = 0; i < numDirs; ++i)
    {
        float angle = randomAngle + (float) i * (2.0f * 3.1415926f / (float) numDirs);
        float2 dir = float2(cos(angle), sin(angle));
        
        // Apply angle bias to prevent surface acne
        float maxAngle = bias;
        
        for (int j = 1; j <= numSteps; ++j)
        {
            float2 offsetUV = input.uv + dir * (stepSizeUV * (float) j);
            
            if (offsetUV.x < 0.0f || offsetUV.x > 1.0f || offsetUV.y < 0.0f || offsetUV.y > 1.0f)
                continue;
            
            float3 S = GetViewPos(offsetUV, texIdx0);
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
    
    ao = 1.0f - saturate((ao / (float) numDirs) * power);
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
            
            float sampleAO = tRawHBAO.SampleLevel(sLinear, sampleUV, 0).r;
            float sampleDepth = tDepth.SampleLevel(sPoint, sampleUV, 0).r;
            float3 sampleNormal = normalize(DecodeGBufferNormal(tNormal.SampleLevel(sPoint, sampleUV, 0).xyz));
            
            // Spatial weight (Distance falloff)
            float spatialWeight = exp(-(x * x + y * y) / (2.0f * 2.0f));
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
