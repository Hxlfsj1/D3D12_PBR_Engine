#ifndef TANGENT_BASIS_HLSLI
#define TANGENT_BASIS_HLSLI

float3 NormalizeSurfaceDirection(float3 value, float3 fallback)
{
    float lengthSquared = dot(value, value);
    if (all(isfinite(value)) && isfinite(lengthSquared) && lengthSquared > 1e-20f)
        return value * rsqrt(lengthSquared);

    float fallbackLengthSquared = dot(fallback, fallback);
    if (all(isfinite(fallback)) && isfinite(fallbackLengthSquared) && fallbackLengthSquared > 1e-20f)
        return fallback * rsqrt(fallbackLengthSquared);
    return float3(0.0f, 0.0f, 1.0f);
}

void BuildOrthonormalTangentBasis(
    float3 normal,
    float3 tangent,
    float3 bitangent,
    out float3 N,
    out float3 T,
    out float3 B)
{
    N = NormalizeSurfaceDirection(normal, float3(0.0f, 0.0f, 1.0f));
    float tangentLengthSquared = dot(tangent, tangent);
    float3 projectedTangent = tangent - dot(tangent, N) * N;
    float projectedLengthSquared = dot(projectedTangent, projectedTangent);
    // Reject missing directions and nearly parallel tangents independently of
    // their scale. Otherwise normalize(0) can poison both screen and world hits.
    if (!all(isfinite(tangent)) || !isfinite(tangentLengthSquared) ||
        tangentLengthSquared <= 1e-20f || !isfinite(projectedLengthSquared) ||
        projectedLengthSquared <= 1e-8f * tangentLengthSquared)
    {
        float3 absoluteNormal = abs(N);
        float3 axis = absoluteNormal.x <= absoluteNormal.y && absoluteNormal.x <= absoluteNormal.z
            ? float3(1.0f, 0.0f, 0.0f)
            : absoluteNormal.y <= absoluteNormal.z
                ? float3(0.0f, 1.0f, 0.0f) : float3(0.0f, 0.0f, 1.0f);
        projectedTangent = axis - dot(axis, N) * N;
        projectedLengthSquared = dot(projectedTangent, projectedTangent);
    }
    T = projectedTangent * rsqrt(projectedLengthSquared);

    // N and T are valid orthogonal unit vectors here. Only the sign is needed
    // from the source bitangent; zero or nonfinite data uses positive handedness.
    float3 orthogonalBitangent = cross(N, T);
    orthogonalBitangent *= rsqrt(dot(orthogonalBitangent, orthogonalBitangent));
    // Remove cancellation error left by Gram-Schmidt near the parallel cutoff.
    T = cross(orthogonalBitangent, N);
    float handedness = all(isfinite(bitangent)) && dot(orthogonalBitangent, bitangent) < 0.0f
        ? -1.0f : 1.0f;
    B = orthogonalBitangent * handedness;
}

#endif
