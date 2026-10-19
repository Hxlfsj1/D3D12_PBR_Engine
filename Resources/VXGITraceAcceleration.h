#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

// Optional acceleration of the complete per-cell triangle CSR. Geometry and
// original reference order are retained: malformed coverage can fall back to
// the original scan, while ordinary rays skip conservative triangle bounds.
// All offsets and escape links address uint words in the existing refs SRV.
class VXGITraceAcceleration
{
public:
    static constexpr uint32_t DenseMarker = 0x80000001u;
    static constexpr uint32_t LinearThreshold = 32;
    static constexpr uint32_t LeafCapacity = 8;
    static constexpr uint32_t NodeWords = 8;
    struct Stats
    {
        uint64_t acceleratedCells = 0;
        uint64_t nodeCount = 0;
        uint64_t leafCount = 0;
        uint64_t maximumCellCandidates = 0;
        uint64_t packedWords = 0;
        double buildMilliseconds = 0;
    };
    struct Packed
    {
        std::vector<uint32_t> offsets;
        std::vector<uint32_t> references;
        Stats stats;
        uint64_t CapacityBytes() const
        {
            return uint64_t(offsets.capacity() + references.capacity()) * sizeof(uint32_t);
        }
    };

    // Each sparse/small cell retains its IDs directly. A dense cell contains:
    //   marker, originalCount, rootWord, original IDs, preorder BVH nodes.
    // A node contains min.xyz/max.xyz float bits, absolute escapeWord, count.
    // count==0 is an internal node whose first child follows its eight words;
    // a leaf is followed by count IDs. Every subtree ends at escapeWord.
    template<class Triangle>
    static Packed Build(const std::vector<uint32_t>& offsets,
        const std::vector<uint32_t>& references, const std::vector<Triangle>& triangles, float cellSize)
    {
        const auto start = std::chrono::steady_clock::now();
        Packed result;
        if (!std::isfinite(cellSize) || cellSize <= 0)
            throw std::invalid_argument("Trace acceleration requires a positive finite cell size.");
        if (offsets.empty())
        {
            if (!references.empty()) throw std::invalid_argument("Trace acceleration has references without offsets.");
            return result;
        }
        if (offsets.front() != 0 || offsets.back() != references.size())
            throw std::invalid_argument("Trace acceleration requires a complete zero-based CSR.");
        bool dense = false;
        uint64_t denseReferences = 0;
        for (size_t cell = 0; cell + 1 < offsets.size(); ++cell)
        {
            if (offsets[cell] > offsets[cell + 1])
                throw std::invalid_argument("Trace acceleration CSR offsets are not monotonic.");
            const uint32_t count = offsets[cell + 1] - offsets[cell];
            result.stats.maximumCellCandidates = (std::max)(result.stats.maximumCellCandidates, uint64_t(count));
            if (count > LinearThreshold) { dense = true; denseReferences += count; }
        }
        for (const uint32_t index : references)
            if (index >= triangles.size() || index >= 0x80000000u)
                throw std::invalid_argument("Trace acceleration triangle index is invalid or overlaps its marker range.");
        if (!dense)
        {
            // Empty vectors explicitly request the unchanged original CSR.
            result.stats.packedWords = references.size();
            result.stats.buildMilliseconds = Milliseconds(start);
            return result;
        }
        const uint64_t reserveWords = uint64_t(references.size()) + denseReferences * 5 + offsets.size() * 3;
        if (reserveWords >= UINT32_MAX)
            throw std::length_error("Trace acceleration exceeds 32-bit word offsets.");
        result.offsets.reserve(offsets.size());
        result.references.reserve(static_cast<size_t>(reserveWords));
        std::vector<Primitive> primitives(triangles.size());
        for (size_t index = 0; index < triangles.size(); ++index)
            primitives[index] = MakePrimitive(triangles[index], cellSize);
        std::vector<uint32_t> order;
        for (size_t cell = 0; cell + 1 < offsets.size(); ++cell)
        {
            result.offsets.push_back(WordOffset(result.references.size()));
            const uint32_t first = offsets[cell], count = offsets[cell + 1] - first;
            if (count <= LinearThreshold)
            {
                result.references.insert(result.references.end(), references.begin() + first, references.begin() + first + count);
                continue;
            }
            ++result.stats.acceleratedCells;
            const size_t header = result.references.size();
            result.references.insert(result.references.end(), {DenseMarker, count, 0});
            result.references.insert(result.references.end(), references.begin() + first, references.begin() + first + count);
            result.references[header + 2] = WordOffset(result.references.size());
            order.assign(references.begin() + first, references.begin() + first + count);
            BuildNode(result, primitives, order, 0, count);
        }
        result.offsets.push_back(WordOffset(result.references.size()));
        result.stats.packedWords = result.references.size();
        result.stats.buildMilliseconds = Milliseconds(start);
        return result;
    }

private:
    struct Primitive
    {
        std::array<double, 3> minimum{}, maximum{}, centroid{};
    };
    static double Milliseconds(std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    static uint32_t WordOffset(size_t value)
    {
        if (value >= UINT32_MAX) throw std::length_error("Trace acceleration exceeds 32-bit word offsets.");
        return static_cast<uint32_t>(value);
    }
    static uint32_t FloatBits(float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }
    static float OutwardFloat(double value, bool maximum)
    {
        const float rounded = static_cast<float>(value);
        const float expanded = std::nextafter(rounded, maximum ?
            std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity());
        if (!std::isfinite(expanded))
            throw std::overflow_error("Trace acceleration bounds exceed finite float coordinates.");
        return expanded;
    }
    template<class Triangle> static Primitive MakePrimitive(const Triangle& triangle, float cellSize)
    {
        Primitive primitive;
        for (size_t axis = 0; axis < 3; ++axis)
        {
            auto component = [axis](const auto& value) { return double(axis == 0 ? value.x : axis == 1 ? value.y : value.z); };
            const double a = component(triangle.positions[0]), b = component(triangle.positions[1]), c = component(triangle.positions[2]);
            if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c))
                throw std::invalid_argument("Trace acceleration requires finite triangle positions.");
            const double scale = (std::max)({std::abs(a), std::abs(b), std::abs(c)});
            // RayTriangle deliberately accepts a 2e-5 barycentric fringe.
            // Bounds must include that fringe as well as float ray arithmetic.
            const double margin = 4e-5 * (std::abs(b - a) + std::abs(c - a)) +
                4 * std::numeric_limits<float>::epsilon() * scale + double(cellSize) * 1e-5;
            primitive.minimum[axis] = (std::min)({a, b, c}) - margin;
            primitive.maximum[axis] = (std::max)({a, b, c}) + margin;
            primitive.centroid[axis] = (a + b + c) / 3;
        }
        return primitive;
    }
    static void BuildNode(Packed& packed, const std::vector<Primitive>& primitives,
        std::vector<uint32_t>& order, uint32_t first, uint32_t count)
    {
        Primitive bounds = primitives[order[first]];
        auto centroidMin = bounds.centroid, centroidMax = bounds.centroid;
        for (uint32_t item = first + 1; item < first + count; ++item)
        {
            const auto& primitive = primitives[order[item]];
            for (size_t axis = 0; axis < 3; ++axis)
            {
                bounds.minimum[axis] = (std::min)(bounds.minimum[axis], primitive.minimum[axis]);
                bounds.maximum[axis] = (std::max)(bounds.maximum[axis], primitive.maximum[axis]);
                centroidMin[axis] = (std::min)(centroidMin[axis], primitive.centroid[axis]);
                centroidMax[axis] = (std::max)(centroidMax[axis], primitive.centroid[axis]);
            }
        }
        const size_t node = packed.references.size();
        packed.references.resize(node + NodeWords);
        ++packed.stats.nodeCount;
        for (size_t axis = 0; axis < 3; ++axis)
        {
            packed.references[node + axis] = FloatBits(OutwardFloat(bounds.minimum[axis], false));
            packed.references[node + 3 + axis] = FloatBits(OutwardFloat(bounds.maximum[axis], true));
        }
        if (count <= LeafCapacity)
        {
            ++packed.stats.leafCount;
            packed.references[node + 7] = count;
            packed.references.insert(packed.references.end(), order.begin() + first, order.begin() + first + count);
        }
        else
        {
            std::array<double, 3> extent{};
            for (size_t axis = 0; axis < 3; ++axis) extent[axis] = centroidMax[axis] - centroidMin[axis];
            const size_t axis = extent[0] >= extent[1] && extent[0] >= extent[2] ? 0 : extent[1] >= extent[2] ? 1 : 2;
            const uint32_t middle = first + count / 2;
            std::nth_element(order.begin() + first, order.begin() + middle, order.begin() + first + count,
                [&](uint32_t a, uint32_t b)
                {
                    const double ca = primitives[a].centroid[axis], cb = primitives[b].centroid[axis];
                    return ca == cb ? a < b : ca < cb;
                });
            BuildNode(packed, primitives, order, first, middle - first);
            BuildNode(packed, primitives, order, middle, first + count - middle);
        }
        packed.references[node + 6] = WordOffset(packed.references.size());
    }
};
