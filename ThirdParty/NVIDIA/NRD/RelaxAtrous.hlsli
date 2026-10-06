/*
Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.

NVIDIA CORPORATION and its licensors retain all intellectual property
and proprietary rights in and to this software, related documentation
and any modifications thereto. Any use, reproduction, disclosure or
distribution of this software and related documentation without an express
license agreement from NVIDIA CORPORATION is strictly prohibited.
*/
// This software contains source code provided by NVIDIA Corporation.
// Adapted RELAX diffuse A-trous weights; source references are in README.md.
#ifndef SSGI_RELAX_ATROUS
#define SSGI_RELAX_ATROUS
float RelaxAtrousNormalWeight(float3 n, float3 m, float fraction)
{
    // RELAX GetNormalWeightParam2 at diffuse roughness=1, followed by ComputeWeight.
    float angleLimit = max(atan(fraction / (1.0 - fraction + 1e-6)), 1.5 / 255.0);
    float angle = acos(clamp(dot(n, m), -1.0, 1.0));
    float t = saturate(1.0 - angle / angleLimit);
    return t * t * (3.0 - 2.0 * t);
}
float RelaxAtrousPlaneWeight(float3 p, float3 n, float3 q, float threshold)
{
    return abs(dot(q - p, n)) < threshold ? 1.0 : 0.0;
}
#endif
