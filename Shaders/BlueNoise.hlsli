#ifndef BLUE_NOISE_HLSLI
#define BLUE_NOISE_HLSLI

#include "../ThirdParty/NVIDIA/STBN/STBNConfig.h"

// Use the consumer's pixel grid (half-resolution for SSGI). Keep XY fixed over time.
// Multiple samples use fixed R2-distributed spatial offsets, following NVIDIA's
// STBN Part 2 guidance. Each ray retains its own 64-frame STBN sequence.
float2 LoadSTBN2D(Texture2DArray<uint2> noise, uint2 pixel, uint frame, uint sampleIndex)
{
    uint2 offset = uint2(frac(float2(0.754877666, 0.569840296) * sampleIndex) *
        float2(STBN_WIDTH, STBN_HEIGHT));
    uint2 texel = (pixel + offset) % uint2(STBN_WIDTH, STBN_HEIGHT);
    uint2 value = noise.Load(int4(texel, frame % STBN_FRAMES, 0));
    // Decode to bin centers in (0,1), with no filtering, sRGB conversion or mipmaps.
    return (float2(value) + 0.5) / 256.0;
}

#endif
