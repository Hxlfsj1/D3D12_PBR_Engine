cbuffer SkyboxData : register(b1)
{
    float4x4 viewProjMat;
    uint skyboxIdx;
};

SamplerState s1 : register(s0);

struct VS_INPUT
{
    float3 pos : POSITION;
};

struct VS_OUTPUT
{
    float4 pos : SV_POSITION;
    float3 texCoord : TEXCOORD;
};

VS_OUTPUT VSMain(VS_INPUT input)
{
    VS_OUTPUT output;
    
    float4 pos = mul(float4(input.pos, 1.0f), viewProjMat);
    
    output.pos = pos.xyww;
    
    output.texCoord = input.pos;
    
    return output;
}

float4 PSMain(VS_OUTPUT input) : SV_TARGET
{
    TextureCube tSkybox = ResourceDescriptorHeap[skyboxIdx];
    
    float3 color = tSkybox.Sample(s1, input.texCoord).rgb;
    return float4(color, 1.0f);
}