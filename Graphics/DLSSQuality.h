#ifndef DLSS_QUALITY_H
#define DLSS_QUALITY_H

#include <array>
#include <string_view>

enum class DLSSQualityMode
{
    DLAA,
    UltraQuality,
    Quality,
    Balanced,
    Performance,
    UltraPerformance
};

struct DLSSOptimalSettings
{
    DLSSQualityMode qualityMode = DLSSQualityMode::Quality;
    unsigned int outputWidth = 0;
    unsigned int outputHeight = 0;
    unsigned int renderWidth = 0;
    unsigned int renderHeight = 0;
    unsigned int maxRenderWidth = 0;
    unsigned int maxRenderHeight = 0;
    unsigned int minRenderWidth = 0;
    unsigned int minRenderHeight = 0;
    float recommendedSharpness = 0.0f;

    bool IsValid() const noexcept
    {
        return outputWidth != 0 &&
            outputHeight != 0 &&
            renderWidth != 0 &&
            renderHeight != 0 &&
            maxRenderWidth != 0 &&
            maxRenderHeight != 0 &&
            minRenderWidth != 0 &&
            minRenderHeight != 0;
    }
};

inline constexpr std::array<DLSSQualityMode, 6> kDLSSQualityModesHighToLow =
{
    DLSSQualityMode::DLAA,
    DLSSQualityMode::UltraQuality,
    DLSSQualityMode::Quality,
    DLSSQualityMode::Balanced,
    DLSSQualityMode::Performance,
    DLSSQualityMode::UltraPerformance
};

inline constexpr const char* GetDLSSQualityModeName(DLSSQualityMode mode) noexcept
{
    switch (mode)
    {
    case DLSSQualityMode::DLAA:
        return "DLAA";
    case DLSSQualityMode::UltraQuality:
        return "UltraQuality";
    case DLSSQualityMode::Quality:
        return "Quality";
    case DLSSQualityMode::Balanced:
        return "Balanced";
    case DLSSQualityMode::Performance:
        return "Performance";
    case DLSSQualityMode::UltraPerformance:
        return "UltraPerformance";
    default:
        return "Unknown";
    }
}

inline bool TryParseDLSSQualityMode(
    std::string_view value,
    DLSSQualityMode* outputMode) noexcept
{
    if (outputMode == nullptr)
    {
        return false;
    }

    for (DLSSQualityMode mode : kDLSSQualityModesHighToLow)
    {
        if (value == GetDLSSQualityModeName(mode))
        {
            *outputMode = mode;
            return true;
        }
    }

    return false;
}

#endif
