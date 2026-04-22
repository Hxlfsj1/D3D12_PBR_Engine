cbuffer SkyboxConstants : register(b1)
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
    float4 clipPos = mul(float4(input.pos, 0.0f), viewProj);
    output.pos = clipPos.xyww;
    return output;
}

TextureCube envMap : register(t0);
SamplerState s1 : register(s0);

float4 PSMain(VS_OUTPUT input) : SV_TARGET
{
    float3 envColor = envMap.Sample(s1, input.localPos).rgb;
    envColor = envColor / (envColor + 1.0);
    
    return float4(envColor, 1.0);
}