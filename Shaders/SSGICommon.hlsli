#ifndef SSGI_COMMON
#define SSGI_COMMON

float3 SSGIViewPosition(float2 uv, float depth, float4x4 invProj)
{
    float4 p = mul(float4(uv * float2(2, -2) + float2(-1, 1), depth, 1), invProj);
    return p.xyz / p.w;
}

#endif
