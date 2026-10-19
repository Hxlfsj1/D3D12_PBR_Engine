#pragma once

#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

// Approximate surface queries over the immutable unsigned distance nodes. The
// interpolation error of a subcell surface can approach sqrt(3)/2 cells, so the
// acceptance threshold is resolution-sized. This is not a triangle hit test.
class VXGISDFThreshold
{
public:
    struct Grid
    {
        DirectX::XMFLOAT3 origin = {};
        float cellSize = 0;
        std::array<uint32_t, 3> cellDimensions = {};
        uint32_t nodeOffset = 0;
        DirectX::XMFLOAT3 boundsMaximum = {};
        uint32_t padding = 0;
    };

    struct Parameters
    {
        float hitThresholdCells = 0.9f;
        float stepScale = 0.9f;
        uint32_t maxSteps = 512;
        uint32_t padding = 0;
    };

    enum class Status : uint32_t { Miss = 0, Hit = 1, Invalid = 2, Exhausted = 3, Stalled = 4 };

    struct Sample
    {
        bool valid = false;
        float distance = 0;
        float uncertainty = 0;
        float safeDistance = 0;
    };

    struct Result
    {
        Status status = Status::Invalid;
        DirectX::XMFLOAT3 position = {};
        float distance = 0;
        float fieldDistance = 0;
        uint32_t sampleCount = 0;
    };

    static bool ValidParameters(const Parameters& parameters)
    {
        return std::isfinite(parameters.hitThresholdCells) && parameters.hitThresholdCells > 0.8660254f &&
            std::isfinite(parameters.stepScale) && parameters.stepScale > 0 && parameters.stepScale <= 1 &&
            parameters.maxSteps > 0;
    }

    static bool NormalizeDirection(DirectX::XMFLOAT3 input, DirectX::XMFLOAT3& normalized)
    {
        normalized = {};
        if (!Finite(input)) return false;
        const float scale = (std::max)({ std::abs(input.x), std::abs(input.y), std::abs(input.z) });
        if (!(scale > 0)) return false;
        const DirectX::XMFLOAT3 scaled = { input.x / scale, input.y / scale, input.z / scale };
        const float length = std::sqrt(scaled.x * scaled.x + scaled.y * scaled.y + scaled.z * scaled.z);
        normalized = { scaled.x / length, scaled.y / length, scaled.z / length };
        return Finite(normalized);
    }

    // Standalone point queries reject out-of-domain positions. They do not
    // extend the field by clamping samples to the border.
    static Sample SampleDistance(const Grid& grid, std::span<const float> nodes, DirectX::XMFLOAT3 position)
    {
        if (!ValidGrid(grid, nodes) || !Inside(grid, position)) return {};
        return SampleUnchecked(grid, nodes, position);
    }

    // Distances use the normalized local direction. An inclusive, zero-width
    // interval is valid. Starting in the threshold shell immediately accepts
    // that interval's first point; this helper has no receiver-specific bias.
    static Result TraceLocal(const Grid& grid, std::span<const float> nodes,
        DirectX::XMFLOAT3 origin, DirectX::XMFLOAT3 direction, float minimum, float maximum,
        const Parameters& parameters)
    {
        Result result;
        DirectX::XMFLOAT3 unitDirection;
        if (!ValidGrid(grid, nodes) || !ValidParameters(parameters) || !Finite(origin) ||
            !NormalizeDirection(direction, unitDirection) || !std::isfinite(minimum) ||
            !std::isfinite(maximum) || minimum < 0 || maximum < minimum)
            return result;
        const float threshold = parameters.hitThresholdCells * grid.cellSize;
        if (!std::isfinite(threshold)) return result;

        float entry = minimum, exit = maximum;
        for (size_t axis = 0; axis < 3; ++axis)
        {
            const float start = Component(origin, axis), delta = Component(unitDirection, axis);
            const float lower = Component(grid.origin, axis), upper = Component(grid.boundsMaximum, axis);
            if (delta == 0)
            {
                if (start < lower || start > upper) { result.status = Status::Miss; return result; }
                continue;
            }
            float a = (lower - start) / delta, b = (upper - start) / delta;
            if (a > b) std::swap(a, b);
            entry = (std::max)(entry, a);
            exit = (std::min)(exit, b);
            if (exit < entry) { result.status = Status::Miss; return result; }
        }

        float current = entry;
        for (uint32_t iteration = 0; iteration < parameters.maxSteps; ++iteration)
        {
            // Slab clipping has already established the domain interval. Clamp
            // only its evaluated position to correct boundary roundoff; the
            // original ray parameter and finite interval remain unchanged.
            const DirectX::XMFLOAT3 rayPosition = {
                origin.x + unitDirection.x * current,
                origin.y + unitDirection.y * current,
                origin.z + unitDirection.z * current };
            if (!Finite(rayPosition)) return result;
            const DirectX::XMFLOAT3 position = {
                std::clamp(rayPosition.x, grid.origin.x, grid.boundsMaximum.x),
                std::clamp(rayPosition.y, grid.origin.y, grid.boundsMaximum.y),
                std::clamp(rayPosition.z, grid.origin.z, grid.boundsMaximum.z) };
            const Sample sample = SampleUnchecked(grid, nodes, position);
            ++result.sampleCount;
            result.position = rayPosition;
            result.distance = current;
            result.fieldDistance = sample.distance;
            if (!sample.valid) return result;
            if (sample.distance <= threshold) { result.status = Status::Hit; return result; }
            if (current >= exit) { result.status = Status::Miss; return result; }

            const float step = parameters.stepScale * sample.safeDistance;
            const float next = (std::min)(current + step, exit);
            if (!(next > current)) { result.status = Status::Stalled; return result; }
            current = next;
        }
        result.status = Status::Exhausted;
        return result;
    }

    static Result TraceLocal(const Grid& grid, std::span<const float> nodes,
        DirectX::XMFLOAT3 origin, DirectX::XMFLOAT3 direction, float minimum, float maximum)
    {
        return TraceLocal(grid, nodes, origin, direction, minimum, maximum, Parameters{});
    }

private:
    static bool Finite(DirectX::XMFLOAT3 value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    static float Component(DirectX::XMFLOAT3 value, size_t axis)
    {
        return axis == 0 ? value.x : axis == 1 ? value.y : value.z;
    }

    static bool Inside(const Grid& grid, DirectX::XMFLOAT3 position)
    {
        return Finite(position) && position.x >= grid.origin.x && position.y >= grid.origin.y &&
            position.z >= grid.origin.z && position.x <= grid.boundsMaximum.x &&
            position.y <= grid.boundsMaximum.y && position.z <= grid.boundsMaximum.z;
    }

    static bool ValidGrid(const Grid& grid, std::span<const float> nodes)
    {
        if (!Finite(grid.origin) || !Finite(grid.boundsMaximum) || !std::isfinite(grid.cellSize) ||
            !(grid.cellSize > 0) || grid.nodeOffset > nodes.size()) return false;
        size_t nodeCount = 1;
        const size_t available = nodes.size() - grid.nodeOffset;
        for (size_t axis = 0; axis < 3; ++axis)
        {
            const uint32_t dimension = grid.cellDimensions[axis];
            if (!dimension || dimension == UINT32_MAX ||
                !(Component(grid.boundsMaximum, axis) > Component(grid.origin, axis))) return false;
            const size_t extent = size_t(dimension) + 1;
            if (nodeCount > available / extent) return false;
            nodeCount *= extent;
            const float upper = Component(grid.origin, axis) + float(dimension) * grid.cellSize;
            if (!std::isfinite(upper)) return false;
        }
        return true;
    }

    static Sample SampleUnchecked(const Grid& grid, std::span<const float> nodes, DirectX::XMFLOAT3 position)
    {
        std::array<uint32_t, 3> cell;
        std::array<float, 3> fraction;
        for (size_t axis = 0; axis < 3; ++axis)
        {
            const float rawCoordinate = (Component(position, axis) - Component(grid.origin, axis)) / grid.cellSize;
            if (!std::isfinite(rawCoordinate)) return {};
            const float coordinate = std::clamp(rawCoordinate, 0.0f, float(grid.cellDimensions[axis]));
            cell[axis] = (std::min)(static_cast<uint32_t>(coordinate), grid.cellDimensions[axis] - 1);
            fraction[axis] = std::clamp(coordinate - float(cell[axis]), 0.0f, 1.0f);
        }
        const size_t width = size_t(grid.cellDimensions[0]) + 1;
        const size_t height = size_t(grid.cellDimensions[1]) + 1;
        const size_t first = grid.nodeOffset + size_t(cell[0]) + width * (size_t(cell[1]) + height * cell[2]);
        std::array<float, 8> values;
        for (uint32_t corner = 0; corner < 8; ++corner)
        {
            values[corner] = nodes[first + (corner & 1u) + width * ((corner >> 1u) & 1u) +
                width * height * ((corner >> 2u) & 1u)];
            if (!std::isfinite(values[corner]) || values[corner] < 0) return {};
        }
        const auto lerp = [](float a, float b, float f) { return a + (b - a) * f; };
        Sample result;
        result.distance = lerp(lerp(lerp(values[0], values[1], fraction[0]), lerp(values[2], values[3], fraction[0]), fraction[1]),
            lerp(lerp(values[4], values[5], fraction[0]), lerp(values[6], values[7], fraction[0]), fraction[1]), fraction[2]);
        result.uncertainty = grid.cellSize * std::sqrt(fraction[0] * (1 - fraction[0]) +
            fraction[1] * (1 - fraction[1]) + fraction[2] * (1 - fraction[2]));
        const float roundoff = (std::max)(grid.cellSize * 1e-4f, std::abs(result.distance) * 2e-6f);
        result.safeDistance = (std::max)(0.0f, result.distance - result.uncertainty - roundoff);
        result.valid = std::isfinite(result.distance) && std::isfinite(result.uncertainty) &&
            std::isfinite(result.safeDistance);
        return result;
    }
};

static_assert(sizeof(VXGISDFThreshold::Grid) == 48);
static_assert(sizeof(VXGISDFThreshold::Parameters) == 16);
