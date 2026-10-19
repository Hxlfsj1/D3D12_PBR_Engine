#pragma once

#include "VXGIWorld.h"
#include <span>

// Sparse event invalidation for fixed world-cell probes. A completed dependency
// mask includes classification, capture and sun-shadow segments, including misses.
// Masks are conservative: a false positive costs an update, a false negative
// would retain stale lighting. They are never inferred from hit surfaces alone.
class VXGIProbeInvalidation
{
public:
    using Mask = std::array<uint32_t, 8>;
    struct Domain
    {
        DirectX::XMFLOAT3 origin = {};
        uint32_t generation = 0;
        DirectX::XMFLOAT3 cellSize = {1, 1, 1};
        uint32_t cellCount = 0;
        std::array<uint32_t, 3> dimensions = {};
        uint32_t padding = 0;
    };
    static_assert(sizeof(Domain) == 48 && sizeof(Mask) == 32);
    struct InstanceSnapshot
    {
        uint32_t id = 0;
        uintptr_t asset = 0;
        DirectX::XMFLOAT3 boundsMinimum = {}, boundsMaximum = {};
        std::array<float, 9> trs = {};
        std::vector<uint32_t> materialIds;
        bool cutout = false;
        uint64_t coverageFingerprint = 0;
    };
    struct Input
    {
        std::span<const VXGIWorld::Cell> cells;
        std::span<const InstanceSnapshot> instances;
        float voxelSize = 0.5f;
        uint64_t topologyRevision = 0;
        uint64_t materialRevision = 0;
        uint64_t lightingRevision = 0;
        // Empty on a payload revision means that all material users may change.
        std::span<const uint32_t> changedMaterialIds;
        std::span<const uint64_t> materialFingerprints;
    };
    struct Change
    {
        bool changed = false, topologyChanged = false, domainChanged = false;
        uint64_t revision = 0;
        std::vector<uint32_t> historyRemap;
        std::vector<uint32_t> dirtyClassification, dirtySH;
    };

    template<class InstanceRange>
    static std::vector<InstanceSnapshot> CaptureInstances(const InstanceRange& instances)
    {
        std::vector<InstanceSnapshot> result;
        result.reserve(instances.size());
        for (const auto& instance : instances)
        {
            InstanceSnapshot value;
            value.id = instance.instanceId;
            value.asset = reinterpret_cast<uintptr_t>(instance.asset.get());
            value.boundsMinimum = instance.boundsMinimum;
            value.boundsMaximum = instance.boundsMaximum;
            value.trs = {instance.translation.x, instance.translation.y, instance.translation.z,
                instance.rotation.x, instance.rotation.y, instance.rotation.z,
                instance.scale.x, instance.scale.y, instance.scale.z};
            value.materialIds = instance.materialIds;
            value.cutout = instance.isCutout;
            result.push_back(std::move(value));
        }
        return result;
    }

    bool Update(const Input& input)
    {
        m_change.changed = m_change.topologyChanged = m_change.domainChanged = false;
        m_change.historyRemap.clear();
        m_change.dirtyClassification.clear();
        m_change.dirtySH.clear();
        if (!std::isfinite(input.voxelSize) || input.voxelSize <= 0 || input.cells.size() > UINT32_MAX)
            return Fail("Probe invalidation requires a finite positive voxel size and 32-bit probe count.");
        for (const auto& instance : input.instances)
            if (!Finite(instance.boundsMinimum) || !Finite(instance.boundsMaximum) ||
                instance.boundsMinimum.x > instance.boundsMaximum.x ||
                instance.boundsMinimum.y > instance.boundsMaximum.y ||
                instance.boundsMinimum.z > instance.boundsMaximum.z)
                return Fail("Probe invalidation instance bounds must be finite and ordered.");

        const bool topologyInputChanged = !m_initialized || input.topologyRevision != m_topologyRevision ||
            input.voxelSize != m_voxelSize || input.cells.size() != m_coordinates.size();
        const bool sameInstances = SameInstances(input.instances, m_instances);
        const bool sameMaterials = input.materialFingerprints.size() == m_materialFingerprints.size() &&
            std::equal(input.materialFingerprints.begin(), input.materialFingerprints.end(), m_materialFingerprints.begin());
        if (m_initialized && !topologyInputChanged && sameInstances &&
            input.materialRevision == m_materialRevision && sameMaterials && input.lightingRevision == m_lightingRevision)
        {
            m_error.clear();
            return true;
        }
        std::vector<VXGIWorld::Coordinate> coordinates;
        coordinates.reserve(input.cells.size());
        for (const auto& cell : input.cells) coordinates.push_back(cell.coordinate);
        const bool topologyChanged = !m_initialized || input.voxelSize != m_voxelSize || coordinates != m_coordinates;
        const bool domainChanged = !m_initialized || input.voxelSize != m_voxelSize || !ContainsAll(input);
        Domain nextDomain = m_domain;
        if (domainChanged)
        {
            nextDomain = BuildDomain(input, m_domain.generation + 1, m_initialized ? &m_domain : nullptr);
            if (nextDomain.generation == 0) nextDomain.generation = 1;
        }

        std::vector<Dependency> dependencies(input.cells.size());
        m_change.historyRemap.assign(input.cells.size(), UINT32_MAX);
        if (m_initialized && input.voxelSize == m_voxelSize)
        {
            std::unordered_map<VXGIWorld::Coordinate, uint32_t, CoordinateHash> oldIndices;
            oldIndices.reserve(m_coordinates.size());
            for (uint32_t i = 0; i < m_coordinates.size(); ++i) oldIndices.emplace(m_coordinates[i], i);
            for (uint32_t i = 0; i < coordinates.size(); ++i)
            {
                const auto found = oldIndices.find(coordinates[i]);
                if (found == oldIndices.end()) continue;
                m_change.historyRemap[i] = found->second;
                dependencies[i] = m_dependencies[found->second];
            }
        }

        Mask geometryMask = {}, materialMask = {};
        std::vector<uint32_t> changedMaterials(input.changedMaterialIds.begin(), input.changedMaterialIds.end());
        for (size_t i = 0; i < (std::max)(input.materialFingerprints.size(), m_materialFingerprints.size()); ++i)
            if (i >= input.materialFingerprints.size() || i >= m_materialFingerprints.size() ||
                input.materialFingerprints[i] != m_materialFingerprints[i])
                changedMaterials.push_back(static_cast<uint32_t>(i));
        bool geometryChanged = false, materialChanged = false;
        const auto addGeometry = [&](const InstanceSnapshot& value)
            { Or(geometryMask, BoxMask(nextDomain, value.boundsMinimum, value.boundsMaximum)); geometryChanged = true; };
        const auto addMaterial = [&](const InstanceSnapshot& value)
            { Or(materialMask, BoxMask(nextDomain, value.boundsMinimum, value.boundsMaximum)); materialChanged = true; };
        std::unordered_map<uint32_t, const InstanceSnapshot*> previous;
        for (const auto& value : m_instances) previous.emplace(value.id, &value);
        for (const auto& value : input.instances)
        {
            const auto found = previous.find(value.id);
            if (found == previous.end()) addGeometry(value);
            else
            {
                const auto& old = *found->second;
                if (!SameGeometry(value, old)) { addGeometry(old); addGeometry(value); }
                else if (value.materialIds != old.materialIds) { addMaterial(old); addMaterial(value); }
                previous.erase(found);
            }
            if (input.materialRevision != m_materialRevision || !sameMaterials)
            {
                bool affected = changedMaterials.empty() && input.materialFingerprints.empty();
                for (const uint32_t material : changedMaterials)
                    affected = affected || std::find(value.materialIds.begin(), value.materialIds.end(), material) != value.materialIds.end();
                if (affected) addMaterial(value);
            }
        }
        for (const auto& value : previous) addGeometry(*value.second);
        const bool allSH = m_initialized && input.lightingRevision != m_lightingRevision;
        for (uint32_t i = 0; i < coordinates.size(); ++i)
        {
            auto& dependency = dependencies[i];
            const bool needsClass = domainChanged || m_change.historyRemap[i] == UINT32_MAX ||
                (geometryChanged && (!dependency.valid || Intersects(dependency.mask, geometryMask)));
            const bool needsSH = allSH || (materialChanged && (!dependency.valid || Intersects(dependency.mask, materialMask)));
            if (needsClass)
            {
                m_change.dirtyClassification.push_back(i);
                dependency = {};
            }
            else if (needsSH) m_change.dirtySH.push_back(i);
        }
        m_coordinates = std::move(coordinates);
        m_dependencies = std::move(dependencies);
        m_instances.assign(input.instances.begin(), input.instances.end());
        m_materialFingerprints.assign(input.materialFingerprints.begin(), input.materialFingerprints.end());
        m_voxelSize = input.voxelSize;
        m_topologyRevision = input.topologyRevision;
        m_materialRevision = input.materialRevision;
        m_lightingRevision = input.lightingRevision;
        m_domain = nextDomain;
        m_initialized = true;
        m_change.changed = true;
        m_change.topologyChanged = topologyChanged;
        m_change.domainChanged = domainChanged;
        m_change.revision = ++m_revision;
        m_error.clear();
        return true;
    }

    // Invoke only after scheduler generation validation. OR retains paths from
    // classification and all successful historical samples, rather than only the
    // latest capture batch. A domain change rejects old asynchronous feedback.
    bool AcceptDependency(uint32_t probeIndex, const Mask& mask, uint32_t domainGeneration)
    {
        if (probeIndex >= m_dependencies.size() || domainGeneration != m_domain.generation) return false;
        auto& dependency = m_dependencies[probeIndex];
        Or(dependency.mask, mask);
        // The separately cached six-neighbor visibility is deterministic and
        // must not rely on stochastic capture rays happening to cover an edge.
        // Include missing neighbors too: a later allocation can create them.
        const auto coordinate = m_coordinates[probeIndex];
        const DirectX::XMFLOAT3 center = {(float(coordinate.x) + 0.5f) * m_voxelSize,
            (float(coordinate.y) + 0.5f) * m_voxelSize, (float(coordinate.z) + 0.5f) * m_voxelSize};
        for (uint32_t axis = 0; axis < 3; ++axis)
            for (const float sign : {-1.0f, 1.0f})
            {
                auto end = center;
                if (axis == 0) end.x += sign * m_voxelSize;
                else if (axis == 1) end.y += sign * m_voxelSize;
                else end.z += sign * m_voxelSize;
                Or(dependency.mask, BoxMask(m_domain,
                    {(std::min)(center.x, end.x), (std::min)(center.y, end.y), (std::min)(center.z, end.z)},
                    {(std::max)(center.x, end.x), (std::max)(center.y, end.y), (std::max)(center.z, end.z)}));
            }
        dependency.valid = true;
        return true;
    }
    bool RecordDependencies(uint32_t probeIndex, const Mask& mask, uint32_t domainGeneration)
        { return AcceptDependency(probeIndex, mask, domainGeneration); }
    // While capture is in flight its new paths are unknown. Keep earlier paths
    // for the completed OR, but treat any geometry event as affecting this slot.
    void MarkDependenciesUnknown(std::span<const uint32_t> indices)
        { for (const uint32_t index : indices) if (index < m_dependencies.size()) m_dependencies[index].valid = false; }
    const Domain& GetDomain() const { return m_domain; }
    const Change& GetChange() const { return m_change; }
    const std::string& GetLastError() const { return m_error; }
    uint64_t GetRevision() const { return m_revision; }
    bool HasDependency(uint32_t probeIndex) const
        { return probeIndex < m_dependencies.size() && m_dependencies[probeIndex].valid; }
    const Mask& GetDependency(uint32_t probeIndex) const { return m_dependencies.at(probeIndex).mask; }

    static bool Intersects(const Mask& a, const Mask& b)
    {
        for (size_t i = 0; i < a.size(); ++i) if ((a[i] & b[i]) != 0) return true;
        return false;
    }
    static void Or(Mask& destination, const Mask& source)
        { for (size_t i = 0; i < destination.size(); ++i) destination[i] |= source[i]; }

    // Closed boxes conservatively touch both cells at boundaries. Linear grid
    // indices are x + nx * (y + ny*z). Extra boundary cells are intentional.
    static Mask BoxMask(const Domain& domain, DirectX::XMFLOAT3 minimum, DirectX::XMFLOAT3 maximum)
    {
        Mask mask = {};
        if (!domain.cellCount || !Finite(minimum) || !Finite(maximum)) return mask;
        const float lows[] = {minimum.x, minimum.y, minimum.z};
        const float highs[] = {maximum.x, maximum.y, maximum.z};
        const float origins[] = {domain.origin.x, domain.origin.y, domain.origin.z};
        const float sizes[] = {domain.cellSize.x, domain.cellSize.y, domain.cellSize.z};
        std::array<int32_t, 3> first = {}, last = {};
        for (size_t axis = 0; axis < 3; ++axis)
        {
            const double low = (double(lows[axis]) - origins[axis]) / sizes[axis];
            const double high = (double(highs[axis]) - origins[axis]) / sizes[axis];
            if (high < 0 || low > domain.dimensions[axis] || low > high) return {};
            const double tolerance = 8.0 * (std::numeric_limits<float>::epsilon)() *
                (1.0 + (std::max)(std::abs(low), std::abs(high)));
            first[axis] = static_cast<int32_t>((std::max)(0.0, std::ceil(low - tolerance) - 1));
            last[axis] = static_cast<int32_t>((std::min)(double(domain.dimensions[axis] - 1), std::floor(high + tolerance)));
        }
        for (int32_t z = first[2]; z <= last[2]; ++z)
            for (int32_t y = first[1]; y <= last[1]; ++y)
                for (int32_t x = first[0]; x <= last[0]; ++x)
                {
                    const uint32_t bit = uint32_t(x) + domain.dimensions[0] * (uint32_t(y) + domain.dimensions[1] * uint32_t(z));
                    mask[bit >> 5] |= 1u << (bit & 31);
                }
        return mask;
    }

private:
    struct Dependency { Mask mask = {}; bool valid = false; };
    struct CoordinateHash
    {
        size_t operator()(const VXGIWorld::Coordinate& coordinate) const noexcept
        {
            uint64_t value = uint32_t(coordinate.x) * UINT64_C(0x9e3779b185ebca87);
            value ^= uint32_t(coordinate.y) * UINT64_C(0xc2b2ae3d27d4eb4f);
            value ^= uint32_t(coordinate.z) * UINT64_C(0x165667b19e3779f9);
            return static_cast<size_t>(value ^ (value >> 32));
        }
    };
    static bool Finite(const DirectX::XMFLOAT3& value)
        { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }
    static bool Equal(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b)
        { return a.x == b.x && a.y == b.y && a.z == b.z; }
    static bool SameGeometry(const InstanceSnapshot& a, const InstanceSnapshot& b)
    {
        return a.id == b.id && a.asset == b.asset && a.trs == b.trs && a.cutout == b.cutout &&
            a.coverageFingerprint == b.coverageFingerprint &&
            Equal(a.boundsMinimum, b.boundsMinimum) && Equal(a.boundsMaximum, b.boundsMaximum);
    }
    static bool SameInstances(std::span<const InstanceSnapshot> a, std::span<const InstanceSnapshot> b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (!SameGeometry(a[i], b[i]) || a[i].materialIds != b[i].materialIds) return false;
        return true;
    }
    bool ContainsAll(const Input& input) const
    {
        const auto contains = [&](DirectX::XMFLOAT3 low, DirectX::XMFLOAT3 high)
        {
            return low.x >= m_domain.origin.x && low.y >= m_domain.origin.y && low.z >= m_domain.origin.z &&
                high.x <= m_domain.origin.x + m_domain.cellSize.x * m_domain.dimensions[0] &&
                high.y <= m_domain.origin.y + m_domain.cellSize.y * m_domain.dimensions[1] &&
                high.z <= m_domain.origin.z + m_domain.cellSize.z * m_domain.dimensions[2];
        };
        for (const auto& value : input.instances) if (!contains(value.boundsMinimum, value.boundsMaximum)) return false;
        for (const auto& cell : input.cells)
        {
            DirectX::XMFLOAT3 point = {(float(cell.coordinate.x) + 0.5f) * input.voxelSize,
                (float(cell.coordinate.y) + 0.5f) * input.voxelSize, (float(cell.coordinate.z) + 0.5f) * input.voxelSize};
            if (!contains(point, point)) return false;
        }
        return true;
    }
    static Domain BuildDomain(const Input& input, uint32_t generation, const Domain* previous)
    {
        DirectX::XMFLOAT3 minimum = {}, maximum = {};
        bool hasBounds = false;
        const auto include = [&](DirectX::XMFLOAT3 low, DirectX::XMFLOAT3 high)
        {
            if (!hasBounds) { minimum = low; maximum = high; hasBounds = true; return; }
            minimum = {(std::min)(minimum.x, low.x), (std::min)(minimum.y, low.y), (std::min)(minimum.z, low.z)};
            maximum = {(std::max)(maximum.x, high.x), (std::max)(maximum.y, high.y), (std::max)(maximum.z, high.z)};
        };
        for (const auto& value : input.instances) include(value.boundsMinimum, value.boundsMaximum);
        for (const auto& cell : input.cells)
        {
            DirectX::XMFLOAT3 point = {(float(cell.coordinate.x) + 0.5f) * input.voxelSize,
                (float(cell.coordinate.y) + 0.5f) * input.voxelSize, (float(cell.coordinate.z) + 0.5f) * input.voxelSize};
            include(point, point);
        }
        if (previous && previous->cellCount)
            include(previous->origin, {previous->origin.x + previous->cellSize.x * previous->dimensions[0],
                previous->origin.y + previous->cellSize.y * previous->dimensions[1],
                previous->origin.z + previous->cellSize.z * previous->dimensions[2]});
        Domain domain;
        domain.generation = generation;
        domain.dimensions = {8, 4, 8};
        domain.cellCount = 256;
        const DirectX::XMFLOAT3 margin = {(std::max)(2 * input.voxelSize, (maximum.x - minimum.x) * 0.25f),
            (std::max)(2 * input.voxelSize, (maximum.y - minimum.y) * 0.25f),
            (std::max)(2 * input.voxelSize, (maximum.z - minimum.z) * 0.25f)};
        domain.origin = {std::floor((minimum.x - margin.x) / input.voxelSize) * input.voxelSize,
            std::floor((minimum.y - margin.y) / input.voxelSize) * input.voxelSize,
            std::floor((minimum.z - margin.z) / input.voxelSize) * input.voxelSize};
        const std::array<float, 3> extent = {maximum.x - domain.origin.x + margin.x,
            maximum.y - domain.origin.y + margin.y, maximum.z - domain.origin.z + margin.z};
        domain.cellSize = {extent[0] / domain.dimensions[0], extent[1] / domain.dimensions[1], extent[2] / domain.dimensions[2]};
        return domain;
    }
    bool Fail(const char* error) { m_error = error; return false; }
    bool m_initialized = false;
    float m_voxelSize = 0;
    uint64_t m_topologyRevision = 0, m_materialRevision = 0, m_lightingRevision = 0, m_revision = 0;
    Domain m_domain;
    Change m_change;
    std::vector<VXGIWorld::Coordinate> m_coordinates;
    std::vector<Dependency> m_dependencies;
    std::vector<InstanceSnapshot> m_instances;
    std::vector<uint64_t> m_materialFingerprints;
    std::string m_error;
};
