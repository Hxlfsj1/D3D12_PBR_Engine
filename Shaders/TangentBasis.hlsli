#ifndef TANGENT_BASIS_HLSLI
#define TANGENT_BASIS_HLSLI

void BuildOrthonormalTangentBasis(
    float3 normal,
    float3 tangent,
    float3 bitangent,
    out float3 N,
    out float3 T,
    out float3 B)
{
    N = normalize(normal);
    T = normalize(tangent - dot(tangent, N) * N);

    float3 rawB = normalize(bitangent);
    float handedness = dot(cross(N, T), rawB) < 0.0f ? -1.0f : 1.0f;
    B = normalize(cross(N, T)) * handedness;
}

#endif
