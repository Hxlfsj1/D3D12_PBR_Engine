// D G F here: 
// D Term: Logic inverted to fetch light rather than receive it
// G Term: Tweaked a single coefficient (k) because IBL naturally aligns with physical laws
// F Term: Features a roughness-clamped version in the main shader composition (Shaders_For_PBR.hlsl)

cbuffer MatrixBuffer : register(b0)
{
    float4x4 viewProj;
};

cbuffer PrefilterBuffer : register(b1)
{
    float roughness;
    float3 padding;
};

TextureCube environmentMap : register(t0);
SamplerState s1 : register(s0);

static const float PI = 3.14159265359;

struct VS_INPUT_CUBE
{
    float3 pos : POSITION;
};

struct VS_OUTPUT_CUBE
{
    float4 pos : SV_POSITION;
    float3 localPos : POSITION;
};

struct VS_INPUT_QUAD
{
    float3 pos : POSITION;
    float2 texCoord : TEXCOORD;
};

struct VS_OUTPUT_QUAD
{
    float4 pos : SV_POSITION;
    float2 texCoord : TEXCOORD;
};

// Generate quasi-random numbers using the Van der Corput Radical Inverse
float RadicalInverse_VdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

// Hammersley Sequence Sampler for Monte Carlo Integration
float2 Hammersley(uint i, uint N)
{
    return float2(float(i) / float(N), RadicalInverse_VdC(i));
}

// NDF in IBL (the D term here isn't for calculating highlights, it determines the sampling spread for importance sampling)
float3 ImportanceSampleGGX(float2 Xi, float3 N, float roughness)
{
    float a = roughness * roughness;
    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    float3 H = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    float3 up = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(up, N));
    float3 bitangent = cross(N, tangent);
    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

// G in IBL (the k value changes because IBL requires physical correctness)
float GeometrySchlickGGX(float NdotV, float roughness)
{
    float k = (roughness * roughness) / 2.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    return GeometrySchlickGGX(max(dot(N, L), 0.0), roughness) * GeometrySchlickGGX(max(dot(N, V), 0.0), roughness);
}

// Prefilter vertex shader
// Pass the local position to PS to serve as the baseline sampling direction
VS_OUTPUT_CUBE VSMain_Prefilter(VS_INPUT_CUBE input)
{
    VS_OUTPUT_CUBE output;
    output.localPos = input.pos;
    output.pos = mul(float4(input.pos, 1.0f), viewProj);
    return output;
}

// Prefilter pixel shader
// Generate the prefiltered environment map
float4 PSMain_Prefilter(VS_OUTPUT_CUBE input) : SV_TARGET
{
    // Assume V = N for high performance; however, this yields poor results at steep grazing angles
    float3 N = normalize(input.localPos);
    float3 R = N;
    float3 V = R;
    
    // Approximating the spherical integral via Monte Carlo integration (the blurriness is driven by roughness)
    const uint SAMPLE_COUNT = 1024u;
    float totalWeight = 0.0;
    float3 prefilteredColor = float3(0.0, 0.0, 0.0);
    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        float2 Xi = Hammersley(i, SAMPLE_COUNT);
        float3 H = ImportanceSampleGGX(Xi, N, roughness);
        float3 L = normalize(2.0 * dot(V, H) * H - V);
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0)
        {
            // This step determines that it is not a pure mathematical computation,
            // but rather a multi-level filtering process of a specific HDR environment map
            prefilteredColor += environmentMap.SampleLevel(s1, L, 0).rgb * NdotL;
            totalWeight += NdotL;
        }
    }
    
    return float4(prefilteredColor / totalWeight, 1.0);
}

// Function to compute the BRDF LUT (Integration Map)
// t is purely a mathematical computation, independent of specific models or HDR data
// Commercial engines rarely implement this at runtime, they simply sample from a pre-computed LUT
float2 IntegrateBRDF(float NdotV, float roughness)
{
    float3 V = float3(sqrt(max(1.0 - NdotV * NdotV, 0.0)), 0.0, NdotV);
    float A = 0.0;
    float B = 0.0;
    float3 N = float3(0.0, 0.0, 1.0);
    
    // It is also a Monte Carlo integration, but the two variables are the incident angle and roughness
    const uint SAMPLE_COUNT = 1024u;
    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        float2 Xi = Hammersley(i, SAMPLE_COUNT);
        float3 H = ImportanceSampleGGX(Xi, N, roughness);
        float3 L = normalize(2.0 * dot(V, H) * H - V);
        float NdotL = max(L.z, 0.0);
        float NdotH = max(H.z, 0.0);
        float VdotH = max(dot(V, H), 0.0);
        if (NdotL > 0.0)
        {
            float G = GeometrySmith(N, V, L, roughness);
            float G_Vis = (G * VdotH) / (NdotH * NdotV);
            float Fc = pow(1.0 - VdotH, 5.0);
            A += (1.0 - Fc) * G_Vis;
            B += Fc * G_Vis;
        }
    }
    
    // The return value A represents Scale and B represents Bias, which adjust the F0 value (F0_new = F0_Origin * Scale + Bias)
    return float2(A / float(SAMPLE_COUNT), B / float(SAMPLE_COUNT));
}

// BRDF LUT vertex shader
// The rectangular canvas for generating the BRDF LUT
VS_OUTPUT_QUAD VSMain_BRDF(VS_INPUT_QUAD input)
{
    VS_OUTPUT_QUAD output;
    output.pos = float4(input.pos, 1.0);
    output.texCoord = input.texCoord;
    return output;
}

// BRDF LUT pixel shader
// Baking the BRDF LUT
float4 PSMain_BRDF(VS_OUTPUT_QUAD input) : SV_TARGET
{
    float2 brdf = IntegrateBRDF(input.texCoord.x, input.texCoord.y);
    return float4(brdf.x, brdf.y, 0.0, 1.0);
}