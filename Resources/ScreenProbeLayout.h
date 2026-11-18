#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

// The screen grid describes placement; the atlas also contains a compact adaptive
// tail. Keep this arithmetic shared by allocation, graph recording and capture.
struct ScreenProbeLayout
{
    static constexpr uint32_t AdaptiveSamplesPerTile = 8;
    uint32_t uniformWidth = 0, uniformHeight = 0, uniformCount = 0;
    uint32_t adaptiveCapacity = 0, atlasWidth = 0, atlasHeight = 0;

    static bool Calculate(uint32_t width, uint32_t height, uint32_t factor,
        bool adaptive, float maxFraction, ScreenProbeLayout& result) noexcept
    {
        result = {};
        if (!width || !height || !factor || !std::isfinite(maxFraction) ||
            maxFraction < 0.0f || maxFraction > 1.0f) return false;
        const uint64_t columns = (uint64_t(width) + factor - 1u) / factor;
        const uint64_t rows = (uint64_t(height) + factor - 1u) / factor;
        if (columns > 2048u || rows > 2048u) return false;
        const uint64_t count = columns * rows;
        const uint64_t extra = adaptive ? uint64_t(std::floor(double(count) * double(maxFraction))) : 0u;
        const uint64_t atlasRows = (count + extra + columns - 1u) / columns;
        // D3D12 2D dimension is 16384; each directional probe occupies 8x8.
        if (columns > 2048u || atlasRows > 2048u || count + extra > UINT32_MAX) return false;
        result = {uint32_t(columns), uint32_t(rows), uint32_t(count), uint32_t(extra),
            uint32_t(columns), uint32_t(atlasRows)};
        return true;
    }

    bool operator==(const ScreenProbeLayout&) const = default;
};
