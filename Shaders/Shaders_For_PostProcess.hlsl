Texture2D sceneTexture : register(t0);
SamplerState s0 : register(s0);

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

float4 PSMain(VS_OUTPUT input) : SV_TARGET
{
    // This HDR doesn't refer to the HDR skybox, but rather the post-processing input awaiting tone mapping
    float4 hdrColor = sceneTexture.Sample(s0, input.uv);
    float3 color = hdrColor.rgb;

    // ACES Filmic Tone Mapping
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    color = saturate((color * (a * color + b)) / (color * (c * color + d) + e));
    color = pow(color, 1.0 / 2.2);
    
    return float4(color, hdrColor.a);
}