#ifndef FULLSCREEN_TRIANGLE_HLSLI
#define FULLSCREEN_TRIANGLE_HLSLI

float2 GetFullscreenTriangleTexCoord(uint vertexID)
{
    return float2((vertexID << 1) & 2, vertexID & 2);
}

float4 GetFullscreenTrianglePosition(float2 texCoord)
{
    return float4(texCoord * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}

#endif
