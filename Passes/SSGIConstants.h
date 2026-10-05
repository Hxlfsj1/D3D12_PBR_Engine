#pragma once

#include <DirectXMath.h>
#include <cstdint>

// Keep these layouts in sync with the SSGI shader cbuffers.
namespace SSGI
{
    constexpr uint32_t MaxLevels = 7; // Reduced mips 1..7; original depth is mip 0.

    struct PyramidConstants
    {
        DirectX::XMFLOAT4X4 invProj;
        uint32_t sourceWidth, sourceHeight, outputWidth, outputHeight;
        uint32_t depthIndex, outputDepthIndex, firstLevel, padding = 0;
    };
    static_assert(sizeof(PyramidConstants) == 96);

    struct TraceConstants
    {
        DirectX::XMFLOAT4X4 view, proj, invProj;
        uint32_t width, height, outputWidth, outputHeight;
        uint32_t depthIndex, normalIndex, ormIndex, sourceIndex;
        uint32_t outputIndex, levelCount, frameIndex, blueNoiseIndex;
        float radius, thickness, normalBias, padding2 = 0;
        uint32_t rayCount = 4, maxIterations = 64;
        uint32_t padding3[2] = {};
        // uint arrays in HLSL cbuffers have 16-byte stride. Use uint4 explicitly.
        DirectX::XMUINT4 levels[MaxLevels]; // depth SRV, padding, padding, padding
    };
    static_assert(sizeof(TraceConstants) == 384);

    struct ReconstructConstants
    {
        DirectX::XMFLOAT4X4 view, invProj;
        uint32_t width, height, outputWidth, outputHeight;
        uint32_t depthIndex, normalIndex, ormIndex, rawIndex;
        uint32_t outputIndex;
        uint32_t padding[3] = {};
    };
    static_assert(sizeof(ReconstructConstants) == 176);

    struct TemporalConstants
    {
        DirectX::XMFLOAT4X4 invViewProj, previousViewProj, previousInvViewProj;
        uint32_t width, height, outputWidth, outputHeight;
        uint32_t depthIndex, normalIndex, ormIndex, currentIndex;
        uint32_t previousGIIndex, previousDepthIndex, previousNormalIndex, historyValid;
        uint32_t outputGIIndex, outputDepthIndex, outputNormalIndex;
        uint32_t padding = 0;
    };
    static_assert(sizeof(TemporalConstants) == 256);

    struct CompositeConstants
    {
        DirectX::XMFLOAT4X4 view, invProj;
        uint32_t width, height, halfWidth, halfHeight;
        uint32_t depthIndex, normalIndex, ormIndex, albedoIndex;
        uint32_t giIndex;
        float intensity = 1.0f;
        uint32_t padding[2] = {};
    };
    static_assert(sizeof(CompositeConstants) == 176);
}
