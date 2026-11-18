#ifndef SSGI_SCREEN_TRACE_COMMON_HLSLI
#define SSGI_SCREEN_TRACE_COMMON_HLSLI

// Hi-Z query result; Surface Cache routing follows a screen miss.
static const uint SSGI_SCREEN_MISS = 0;
static const uint SSGI_SCREEN_HIT = 1;
static const uint SSGI_SCREEN_OUT_OF_SCREEN = 2;
static const uint SSGI_SCREEN_RADIUS_LIMIT = 3;
static const uint SSGI_SCREEN_ITERATION_LIMIT = 4;
static const uint SSGI_SCREEN_INVALID = 5;

struct SSGIScreenRayResult
{
    float3 viewPoint; float distance;
    uint2 hitPixel; uint status; float confidence;
    float3 rayPoint; float padding;
};

bool SSGINormalizeDirection(float3 direction, out float3 unitDirection)
{
    unitDirection = 0.0;
    float maximum = max(abs(direction.x), max(abs(direction.y), abs(direction.z)));
    if (!all(isfinite(direction)) || maximum == 0.0) return false;
    float3 scaled = direction / maximum;
    float lengthSquared = dot(scaled, scaled);
    if (!isfinite(lengthSquared) || lengthSquared <= 0.0) return false;
    unitDirection = scaled * rsqrt(lengthSquared);
    return all(isfinite(unitDirection));
}

#endif
