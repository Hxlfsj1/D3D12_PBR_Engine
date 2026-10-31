#include "../Shaders/TangentBasis.hlsli"

struct BasisInput
{
    float4 normal;
    float4 tangent;
    float4 bitangent;
};

struct BasisOutput
{
    float4 normal;
    float4 tangent;
    float4 bitangent;
    float4 mappedNormal;
};

StructuredBuffer<BasisInput> Inputs : register(t0);
RWStructuredBuffer<BasisOutput> Outputs : register(u0);
cbuffer TestConstants : register(b0) { uint CaseCount; }

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= CaseCount) return;
    BasisInput source = Inputs[id.x];
    float3 N, T, B;
    BuildOrthonormalTangentBasis(source.normal.xyz, source.tangent.xyz,
        source.bitangent.xyz, N, T, B);
    BasisOutput result;
    result.normal = float4(N, 0);
    result.tangent = float4(T, 0);
    result.bitangent = float4(B, 0);
    // Match the real G-buffer path's dummy (128,128,255) normal texture,
    // including its flipped green channel. A broken basis must remain visible.
    float3 defaultNormal = float3(128.0 / 255.0, 128.0 / 255.0, 1) * 2 - 1;
    defaultNormal.y = -defaultNormal.y;
    result.mappedNormal = float4(normalize(mul(defaultNormal, float3x3(T, B, N))), 0);
    Outputs[id.x] = result;
}
