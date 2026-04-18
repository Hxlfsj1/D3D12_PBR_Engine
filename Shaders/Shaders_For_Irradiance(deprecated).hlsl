cbuffer MatrixBuffer : register(b0)
{
    float4x4 viewProj;
};

struct VS_INPUT
{
    float3 pos : POSITION;
};

struct VS_OUTPUT
{
    float4 pos : SV_POSITION;
    float3 localPos : POSITION;
};

VS_OUTPUT VSMain(VS_INPUT input)
{
    VS_OUTPUT output;
    output.localPos = input.pos;
    output.pos = mul(float4(input.pos, 1.0f), viewProj);
    return output;
}

TextureCube environmentMap : register(t0);
SamplerState s1 : register(s0);

static const float PI = 3.14159265359;

float4 PSMain(VS_OUTPUT input) : SV_TARGET
{   
    // Retrieve the pixel normal
    float3 N = normalize(input.localPos);
    
    float3 irradiance = float3(0.0, 0.0, 0.0);
    
    // Construct a local coordinate system based on this normal
    float3 up = float3(0.0, 1.0, 0.0);
    float3 right = normalize(cross(up, N));
    up = normalize(cross(N, right));
    
    // Compute the hemispherical integral using Riemann sums
    float sampleDelta = 0.025;
    float nrSamples = 0.0;
    for (float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta)
    {
        for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta)
        {   
            // Transform the ray from local space to world space
            float3 tangentSample = float3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            float3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N;
            
            // Sample texels from the cubemap and aggregate the colors
            irradiance += environmentMap.SampleLevel(s1, sampleVec, 0).rgb * cos(theta) * sin(theta);
            nrSamples++;
        }
    }
    
    return float4(PI * irradiance * (1.0 / nrSamples), 1.0);
}