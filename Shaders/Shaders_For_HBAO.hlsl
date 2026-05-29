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

struct VS_OUTPUT
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};

VS_OUTPUT VSMain(uint vertexID : SV_VertexID)
{
    VS_OUTPUT output;
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.pos = float4(output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

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

float rand(float2 uv)
{
    return frac(sin(dot(uv, float2(12.9898f, 78.233f))) * 43758.5453f);
}

float4 PSMain_HBAO(VS_OUTPUT input) : SV_TARGET
{
    Texture2D tNormal = ResourceDescriptorHeap[texIdx1];
    float3 worldNormal = tNormal.SampleLevel(sPoint, input.uv, 0).xyz * 2.0f - 1.0f;
    float3 viewNormal = normalize(mul(worldNormal, (float3x3) viewMat));

    float3 P = GetViewPos(input.uv, texIdx0);
    
    float randomAngle = rand(input.uv) * 3.1415926f * 2.0f;
    
    int numDirs = 4;
    int numSteps = 4;
    float ao = 0.0f;
    
    float stepSizeUV = (radius / P.z) / (float) numSteps;
    stepSizeUV = clamp(stepSizeUV, 0.001f, 0.05f);
    
    for (int i = 0; i < numDirs; ++i)
    {
        float angle = randomAngle + (float) i * (2.0f * 3.1415926f / (float) numDirs);
        float2 dir = float2(cos(angle), sin(angle));
        
        float maxAngle = bias;
        
        for (int j = 1; j <= numSteps; ++j)
        {
            float2 offsetUV = input.uv + dir * (stepSizeUV * (float) j);
            
            if (offsetUV.x < 0.0f || offsetUV.x > 1.0f || offsetUV.y < 0.0f || offsetUV.y > 1.0f)
                continue;
            
            float3 S = GetViewPos(offsetUV, texIdx0);
            float3 V = S - P;
            float dist = length(V);
            
            if (dist < radius)
            {
                float currentAngle = dot(normalize(V), viewNormal);
                if (currentAngle > maxAngle)
                {
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

float4 PSMain_Blur(VS_OUTPUT input) : SV_TARGET
{
    Texture2D tRawHBAO = ResourceDescriptorHeap[texIdx0];
    Texture2D tDepth = ResourceDescriptorHeap[texIdx1];
    Texture2D tNormal = ResourceDescriptorHeap[texIdx2];

    float centerDepth = tDepth.SampleLevel(sPoint, input.uv, 0).r;
    float3 centerNormal = tNormal.SampleLevel(sPoint, input.uv, 0).xyz;
    
    float result = 0.0f;
    float weightSum = 0.0f;
    float2 texelSize = float2(1.0f / resolutionX, 1.0f / resolutionY);
    
    for (int x = -2; x <= 2; ++x)
    {
        for (int y = -2; y <= 2; ++y)
        {
            float2 offset = float2((float) x, (float) y) * texelSize;
            float2 sampleUV = input.uv + offset;
            
            float sampleAO = tRawHBAO.SampleLevel(sLinear, sampleUV, 0).r;
            float sampleDepth = tDepth.SampleLevel(sPoint, sampleUV, 0).r;
            float3 sampleNormal = tNormal.SampleLevel(sPoint, sampleUV, 0).xyz;
            
            float spatialWeight = exp(-(x * x + y * y) / (2.0f * 2.0f));
            float depthWeight = exp(-abs(centerDepth - sampleDepth) * 100.0f);
            float normalWeight = pow(max(dot(centerNormal, sampleNormal), 0.0f), 16.0f);
            
            float weight = spatialWeight * depthWeight * normalWeight;
            
            result += sampleAO * weight;
            weightSum += weight;
        }
    }
    
    result /= max(weightSum, 0.0001f);
    return float4(result, result, result, 1.0f);
}