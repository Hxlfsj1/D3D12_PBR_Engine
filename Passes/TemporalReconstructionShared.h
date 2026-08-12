#ifndef TEMPORAL_RECONSTRUCTION_SHARED_H
#define TEMPORAL_RECONSTRUCTION_SHARED_H

#include "stdafx.h"

namespace TemporalReconstruction
{
    inline DirectX::XMFLOAT2 CalculateJitter(UINT frameIndex) noexcept
    {
        static constexpr float haltonX[8] = {
            0.5f, 0.25f, 0.75f, 0.125f, 0.625f, 0.375f, 0.875f, 0.0625f
        };
        static constexpr float haltonY[8] = {
            0.333333f, 0.666667f, 0.111111f, 0.444444f,
            0.777778f, 0.222222f, 0.555556f, 0.888889f
        };
        constexpr float jitterScale = 0.75f;
        const UINT sampleIndex = frameIndex % 8u;
        return DirectX::XMFLOAT2(
            (haltonX[sampleIndex] - 0.5f) * jitterScale,
            (haltonY[sampleIndex] - 0.5f) * jitterScale);
    }
}

#endif
