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

Texture2D equirectangularMap : register(t0);
SamplerState s1 : register(s0);

static const float2 invAtan = float2(0.1591, 0.3183);

float2 SampleSphericalMap(float3 v)
{
    float2 uv = float2(atan2(v.z, v.x), asin(v.y));
    uv *= invAtan;
    uv += 0.5;
    return uv;
}

float4 PSMain(VS_OUTPUT input) : SV_TARGET
{
    return float4(equirectangularMap.SampleLevel(s1, SampleSphericalMap(normalize(input.localPos)), 0).rgb, 1.0);
}