#ifndef MATH_COMMON_HLSLI
#define MATH_COMMON_HLSLI

static const float PI = 3.14159265359;

float Rand(float2 co)
{
    return frac(sin(dot(co.xy, float2(12.9898, 78.233))) * 43758.5453);
}

#endif
