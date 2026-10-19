#pragma once

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

// Scene-level broad phase over model-instance AABBs. No triangles or distance
// fields are owned here. Leaves retain indices into the caller's instance table.
class VXGIInstanceBVH
{
public:
    struct Node
    {
        DirectX::XMFLOAT3 boundsMinimum = {};
        uint32_t instanceIndex = UINT32_MAX; // UINT32_MAX denotes an interior node.
        DirectX::XMFLOAT3 boundsMaximum = {};
        uint32_t escapeIndex = 0; // First node after this subtree, in preorder.
    };
    static_assert(sizeof(Node) == 32, "Instance BVH nodes must match their GPU layout.");
    static_assert(offsetof(Node, boundsMaximum) == 16, "Instance BVH bounds must occupy two 16-byte lanes.");

    struct Stats
    {
        uint64_t instanceCount = 0, nodeCount = 0;
        uint64_t builds = 0, refits = 0, noops = 0;
        bool lastUpdateRebuilt = false, lastUpdateRefit = false;
    };

    struct QueryStats
    {
        uint64_t nodeTests = 0, leafTests = 0;
        uint64_t intersectedLeaves = 0, prunedNodes = 0;
    };

    struct Candidate
    {
        uint32_t instanceIndex = UINT32_MAX, instanceId = 0;
        double entryDistance = 0, exitDistance = 0;
    };

    struct QueryResult
    {
        bool valid = false;
        std::array<Candidate, 2> candidates = {};
        uint32_t count = 0;
    };

    // An unchanged scene performs no allocation or tree work. Identity/order
    // changes rebuild; changed bounds preserve topology and refit bottom-up.
    // All validation and allocations precede commit, preserving the previous
    // usable tree and revision on failure. Material fields are never consulted.
    template<class InstanceRange>
    bool Update(const InstanceRange& instances)
    {
        const size_t count = instances.size();
        const uint64_t maximumInstances = (uint64_t(UINT32_MAX) + 1) / 2;
        if (count > maximumInstances)
        {
            m_error = "Instance BVH exceeds its 32-bit node index range.";
            return false;
        }
        bool rebuild = !m_initialized || count != m_records.size();
        bool changedBounds = false;
        for (size_t i = 0; i < count; ++i)
        {
            const auto& instance = instances[i];
            if (!ValidBounds(instance.boundsMinimum, instance.boundsMaximum))
            {
                m_error = "Instance BVH requires finite, ordered world-space AABBs.";
                return false;
            }
            if (i >= m_records.size()) continue;
            rebuild = rebuild || instance.instanceId != m_records[i].instanceId;
            changedBounds = changedBounds || !Same(instance.boundsMinimum, m_records[i].boundsMinimum) ||
                !Same(instance.boundsMaximum, m_records[i].boundsMaximum);
        }
        if (!rebuild && !changedBounds)
        {
            m_stats.lastUpdateRebuilt = false;
            m_stats.lastUpdateRefit = false;
            ++m_stats.noops;
            m_error.clear();
            return true;
        }
        try
        {
            std::vector<Record> records;
            records.reserve(count);
            for (size_t i = 0; i < count; ++i)
            {
                const auto& instance = instances[i];
                records.push_back({instance.boundsMinimum, instance.boundsMaximum, instance.instanceId});
            }
            std::vector<Node> nodes;
            if (rebuild)
            {
                if (count)
                {
                    nodes.reserve(count * 2 - 1);
                    std::vector<uint32_t> order(count);
                    std::iota(order.begin(), order.end(), uint32_t(0));
                    Build(nodes, records, order, 0, count);
                }
            }
            else
            {
                nodes = m_nodes;
                Refit(nodes, records);
            }
            m_records.swap(records);
            m_nodes.swap(nodes);
            m_initialized = true;
            ++m_revision;
            m_stats.instanceCount = count;
            m_stats.nodeCount = m_nodes.size();
            m_stats.lastUpdateRebuilt = rebuild;
            m_stats.lastUpdateRefit = !rebuild;
            if (rebuild) ++m_stats.builds;
            else ++m_stats.refits;
            m_error.clear();
            return true;
        }
        catch (const std::exception& exception)
        {
            m_error = std::string("Instance BVH update failed: ") + exception.what();
            return false;
        }
    }

    const std::vector<Node>& GetNodes() const { return m_nodes; }
    const Stats& GetStats() const { return m_stats; }
    uint64_t GetRevision() const { return m_revision; }
    const std::string& GetLastError() const { return m_error; }

    // The original finite world ray is normalized once; distances remain in
    // world units. AABBs are closed and entry/exit are clipped to [min, max],
    // including zero-width segments and origins already inside a model bound.
    // This allocation-free query selects bounds, not model surface hits.
    QueryResult QueryClosestTwo(const DirectX::XMFLOAT3& origin,
        const DirectX::XMFLOAT3& direction, float rayMin, float rayMax,
        QueryStats* queryStats = nullptr) const
    {
        if (queryStats) *queryStats = {};
        QueryResult result;
        if (!Finite(origin) || !Finite(direction) || !std::isfinite(rayMin) ||
            !std::isfinite(rayMax) || rayMin < 0 || rayMax < rayMin) return result;
        const double length = std::sqrt(double(direction.x) * direction.x +
            double(direction.y) * direction.y + double(direction.z) * direction.z);
        if (length == 0) return result;
        const std::array<double, 3> unit = {direction.x / length, direction.y / length, direction.z / length};
        result.valid = true;
        uint32_t index = 0;
        while (size_t(index) < m_nodes.size())
        {
            const Node& node = m_nodes[index];
            const bool leaf = node.instanceIndex != UINT32_MAX;
            if (queryStats)
            {
                ++queryStats->nodeTests;
                if (leaf) ++queryStats->leafTests;
            }
            double entry, exit;
            if (!RayBox(origin, unit, node.boundsMinimum, node.boundsMaximum, rayMin, rayMax, entry, exit))
            {
                index = node.escapeIndex;
                continue;
            }
            if (queryStats && leaf) ++queryStats->intersectedLeaves;
            // Strictly greater preserves equal-entry candidates with smaller
            // stable IDs, regardless of the median tree's traversal order.
            if (result.count == 2 && entry > result.candidates[1].entryDistance)
            {
                if (queryStats) ++queryStats->prunedNodes;
                index = node.escapeIndex;
                continue;
            }
            if (leaf)
            {
                Candidate candidate;
                candidate.instanceIndex = node.instanceIndex;
                candidate.instanceId = m_records[node.instanceIndex].instanceId;
                candidate.entryDistance = entry;
                candidate.exitDistance = exit;
                Insert(result, candidate);
            }
            ++index;
        }
        return result;
    }

private:
    struct Record
    {
        DirectX::XMFLOAT3 boundsMinimum, boundsMaximum;
        uint32_t instanceId;
    };

    static bool Finite(const DirectX::XMFLOAT3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }
    static bool ValidBounds(const DirectX::XMFLOAT3& minimum, const DirectX::XMFLOAT3& maximum)
    {
        return Finite(minimum) && Finite(maximum) &&
            minimum.x <= maximum.x && minimum.y <= maximum.y && minimum.z <= maximum.z;
    }
    static bool Same(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b)
    {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
    static float Component(const DirectX::XMFLOAT3& value, uint32_t axis)
    {
        return axis == 0 ? value.x : axis == 1 ? value.y : value.z;
    }
    static double Center(const Record& record, uint32_t axis)
    {
        return (double(Component(record.boundsMinimum, axis)) + Component(record.boundsMaximum, axis)) * 0.5;
    }
    static void Merge(Node& parent, const Node& left, const Node& right)
    {
        parent.boundsMinimum = {(std::min)(left.boundsMinimum.x, right.boundsMinimum.x),
            (std::min)(left.boundsMinimum.y, right.boundsMinimum.y),
            (std::min)(left.boundsMinimum.z, right.boundsMinimum.z)};
        parent.boundsMaximum = {(std::max)(left.boundsMaximum.x, right.boundsMaximum.x),
            (std::max)(left.boundsMaximum.y, right.boundsMaximum.y),
            (std::max)(left.boundsMaximum.z, right.boundsMaximum.z)};
    }
    static uint32_t Build(std::vector<Node>& nodes, const std::vector<Record>& records,
        std::vector<uint32_t>& order, size_t begin, size_t end)
    {
        const uint32_t index = static_cast<uint32_t>(nodes.size());
        nodes.emplace_back();
        if (end - begin == 1)
        {
            const uint32_t instanceIndex = order[begin];
            nodes[index].boundsMinimum = records[instanceIndex].boundsMinimum;
            nodes[index].boundsMaximum = records[instanceIndex].boundsMaximum;
            nodes[index].instanceIndex = instanceIndex;
        }
        else
        {
            std::array<double, 3> minimum = {Center(records[order[begin]], 0),
                Center(records[order[begin]], 1), Center(records[order[begin]], 2)};
            auto maximum = minimum;
            for (size_t i = begin + 1; i < end; ++i)
                for (uint32_t axis = 0; axis < 3; ++axis)
                {
                    const double center = Center(records[order[i]], axis);
                    minimum[axis] = (std::min)(minimum[axis], center);
                    maximum[axis] = (std::max)(maximum[axis], center);
                }
            uint32_t axis = 0;
            if (maximum[1] - minimum[1] > maximum[axis] - minimum[axis]) axis = 1;
            if (maximum[2] - minimum[2] > maximum[axis] - minimum[axis]) axis = 2;
            const size_t middle = begin + (end - begin) / 2;
            std::nth_element(order.begin() + begin, order.begin() + middle, order.begin() + end,
                [&](uint32_t a, uint32_t b)
                {
                    const double ca = Center(records[a], axis), cb = Center(records[b], axis);
                    if (ca != cb) return ca < cb;
                    if (records[a].instanceId != records[b].instanceId) return records[a].instanceId < records[b].instanceId;
                    return a < b;
                });
            const uint32_t left = Build(nodes, records, order, begin, middle);
            const uint32_t right = Build(nodes, records, order, middle, end);
            Merge(nodes[index], nodes[left], nodes[right]);
        }
        nodes[index].escapeIndex = static_cast<uint32_t>(nodes.size());
        return index;
    }
    static void Refit(std::vector<Node>& nodes, const std::vector<Record>& records)
    {
        for (size_t remaining = nodes.size(); remaining; --remaining)
        {
            const size_t index = remaining - 1;
            Node& node = nodes[index];
            if (node.instanceIndex != UINT32_MAX)
            {
                node.boundsMinimum = records[node.instanceIndex].boundsMinimum;
                node.boundsMaximum = records[node.instanceIndex].boundsMaximum;
            }
            else
            {
                const size_t left = index + 1;
                const uint32_t right = nodes[left].escapeIndex;
                Merge(node, nodes[left], nodes[right]);
            }
        }
    }
    static bool RayBox(const DirectX::XMFLOAT3& origin, const std::array<double, 3>& direction,
        const DirectX::XMFLOAT3& minimum, const DirectX::XMFLOAT3& maximum,
        double rayMin, double rayMax, double& entry, double& exit)
    {
        entry = rayMin;
        exit = rayMax;
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            const double o = Component(origin, axis);
            const double low = Component(minimum, axis), high = Component(maximum, axis);
            if (direction[axis] == 0)
            {
                if (o < low || o > high) return false;
                continue;
            }
            double a = (low - o) / direction[axis], b = (high - o) / direction[axis];
            if (a > b) std::swap(a, b);
            entry = (std::max)(entry, a);
            exit = (std::min)(exit, b);
            if (entry > exit) return false;
        }
        return true;
    }
    static bool Before(const Candidate& a, const Candidate& b)
    {
        if (a.entryDistance != b.entryDistance) return a.entryDistance < b.entryDistance;
        if (a.instanceId != b.instanceId) return a.instanceId < b.instanceId;
        return a.instanceIndex < b.instanceIndex;
    }
    static void Insert(QueryResult& result, const Candidate& candidate)
    {
        if (result.count == 0)
        {
            result.candidates[0] = candidate;
            result.count = 1;
        }
        else if (Before(candidate, result.candidates[0]))
        {
            result.candidates[1] = result.candidates[0];
            result.candidates[0] = candidate;
            result.count = 2;
        }
        else if (result.count == 1 || Before(candidate, result.candidates[1]))
        {
            result.candidates[1] = candidate;
            result.count = 2;
        }
    }

    std::vector<Node> m_nodes;
    std::vector<Record> m_records;
    Stats m_stats;
    uint64_t m_revision = 0;
    bool m_initialized = false;
    std::string m_error;
};
