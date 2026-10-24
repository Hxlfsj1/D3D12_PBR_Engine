#pragma once

#include "VXGIGlobalSDF.h"
#include <cstring>
#include <utility>

// CPU scheduling state for a toroidally addressed Global SDF. Prepare never
// advances the texture's origin or scene snapshot. Commit is called only after
// the corresponding writes have been submitted to the GPU; Cancel is safe
// after an abandoned command list or a failed resource allocation.
class VXGIGlobalSDFState
{
public:
    using Global = VXGIGlobalSDF;
    using Layout = Global::Layout;
    using Region = Global::UpdateRegion;
    struct Plan
    {
        Layout layout;
        std::vector<Region> regions;
        uint64_t updatedNodes = 0, frameId = 0;
        bool recreate = false;
        std::array<bool, Global::MaximumClipmaps> fullUpdate = {}, updated = {};
    };

    bool Prepare(const DirectX::XMFLOAT3& center, const Global::Settings& settings,
        const VXGIMeshSDFScene& scene, uint64_t frameId, uint32_t assetCount,
        uint32_t sourceNodeCount, std::string* error = nullptr)
    {
        if (error) error->clear();
        // A replacement preparation starts from committed state, including any
        // dirty bounds that a distant clipmap has not reached its turn to apply.
        Cancel();
        if (scene.GetInstances().size() > UINT32_MAX)
            return Fail(error, "Global SDF instance table exceeds 32-bit indexing.");
        Pending next;
        auto& plan = next.plan;
        if (!Global::BuildLayout(center, settings, assetCount,
            uint32_t(scene.GetInstances().size()), sourceNodeCount, plan.layout, error)) return false;
        plan.frameId = frameId;
        plan.recreate = !m_initialized || plan.layout.width != m_layout.width ||
            plan.layout.height != m_layout.height || plan.layout.depth != m_layout.depth;
        next.settings = settings;
        next.signatures.reserve(scene.GetInstances().size());
        for (const auto& instance : scene.GetInstances())
            next.signatures.push_back({instance.instanceId, instance.asset,
                instance.worldFromLocal, {instance.boundsMinimum, instance.boundsMaximum}});
        std::sort(next.signatures.begin(), next.signatures.end(),
            [](const Signature& a, const Signature& b) { return a.instanceId < b.instanceId; });
        for (size_t index = 1; index < next.signatures.size(); ++index)
            if (next.signatures[index - 1].instanceId == next.signatures[index].instanceId)
                return Fail(error, "Global SDF scheduling requires unique instance IDs.");

        bool allFull = !m_initialized || !SameSettings(settings, m_settings) ||
            assetCount != m_layout.constants.assetCount || sourceNodeCount != m_layout.constants.nodeCount;
        next.dirty = m_dirty;
        size_t oldIndex = 0, newIndex = 0;
        while (oldIndex < m_signatures.size() || newIndex < next.signatures.size())
        {
            const Signature* old = oldIndex < m_signatures.size() ? &m_signatures[oldIndex] : nullptr;
            const Signature* current = newIndex < next.signatures.size() ? &next.signatures[newIndex] : nullptr;
            if (old && (!current || old->instanceId < current->instanceId))
            {
                AddDirty(next.dirty, old->bounds); ++oldIndex;
            }
            else if (current && (!old || current->instanceId < old->instanceId))
            {
                AddDirty(next.dirty, current->bounds); ++newIndex;
            }
            else
            {
                if (old->asset != current->asset) allFull = true;
                if (!SameGeometry(*old, *current))
                {
                    AddDirty(next.dirty, old->bounds);
                    AddDirty(next.dirty, current->bounds);
                }
                ++oldIndex; ++newIndex;
            }
        }
        if (m_initialized && !allFull)
            for (uint32_t layer = 0; layer < settings.clipmapCount; ++layer)
            {
                const auto& old = m_layout.constants.clipmaps[layer];
                const auto& current = plan.layout.constants.clipmaps[layer];
                const double distance[] = {double(current.origin.x) - old.origin.x,
                    double(current.origin.y) - old.origin.y, double(current.origin.z) - old.origin.z};
                for (double value : distance)
                    if (std::abs(value) >= double(Global::Resolution(plan.layout.constants, layer) - 1) * current.cellSize)
                        allFull = true;
            }

        for (uint32_t layer = 0; layer < settings.clipmapCount; ++layer)
        {
            const uint32_t resolution = Global::Resolution(plan.layout.constants, layer);
            const uint64_t layerNodes = uint64_t(resolution) * resolution * resolution;
            auto& clipmap = plan.layout.constants.clipmaps[layer];
            // Cadence can defer the outer box only while it still contains the
            // already selected inner box. Propagate an early update outward.
            const bool preserveNesting = !allFull && layer &&
                !Contains(m_layout.constants.clipmaps[layer], plan.layout.constants.clipmaps[layer - 1]);
            if (!allFull && !preserveNesting && frameId % (uint64_t(1) << layer) != 0)
            {
                clipmap = m_layout.constants.clipmaps[layer];
                continue;
            }
            plan.updated[layer] = true;
            const size_t regionStart = plan.regions.size();
            if (allFull || next.dirty[layer].full)
                AppendFull(plan, layer);
            else
            {
                const auto& previous = m_layout.constants.clipmaps[layer];
                const auto delta = CellDelta(previous, clipmap);
                for (uint32_t axis = 0; axis < 3; ++axis)
                    clipmap.scrollOffset[axis] = Mod(int64_t(previous.scrollOffset[axis]) + delta[axis], resolution);
                AppendSlabs(plan, layer, delta);
                for (const auto& bounds : next.dirty[layer].bounds) AppendBounds(plan, layer, bounds);
                uint64_t writes = 0;
                for (size_t index = regionStart; index < plan.regions.size(); ++index) writes += Count(plan.regions[index]);
                // Overlapping dirty boxes are inexpensive to collect. Collapse
                // them before their dispatch cost approaches a complete layer.
                if (plan.regions.size() - regionStart > MaximumRegionsPerLayer || writes >= layerNodes * 3 / 4)
                {
                    plan.regions.resize(regionStart);
                    AppendFull(plan, layer);
                }
            }
            next.dirty[layer] = {};
        }
        for (const auto& region : plan.regions) plan.updatedNodes += Count(region);
        m_pending = std::move(next);
        m_hasPending = true;
        return true;
    }

    const Plan& GetPendingPlan() const { return m_pending.plan; }
    bool HasPendingPlan() const { return m_hasPending; }
    bool IsInitialized() const { return m_initialized; }
    const Layout& GetLayout() const { return m_layout; }
    bool Commit()
    {
        if (!m_hasPending) return false;
        m_layout = m_pending.plan.layout;
        m_settings = m_pending.settings;
        m_signatures = std::move(m_pending.signatures);
        m_dirty = std::move(m_pending.dirty);
        m_initialized = true;
        Cancel();
        return true;
    }
    void Cancel() { m_pending = {}; m_hasPending = false; }
    void Reset()
    {
        Cancel(); m_layout = {}; m_settings = {}; m_signatures.clear();
        m_dirty = {}; m_initialized = false;
    }

private:
    static constexpr size_t MaximumRegionsPerLayer = 32;
    struct Bounds { DirectX::XMFLOAT3 minimum, maximum; };
    struct Signature
    {
        uint32_t instanceId;
        VXGIMeshSDFGPUData::AssetOwner asset;
        DirectX::XMFLOAT4X4 worldFromLocal;
        Bounds bounds;
    };
    struct Dirty { std::vector<Bounds> bounds; bool full = false; };
    using DirtyLayers = std::array<Dirty, Global::MaximumClipmaps>;
    struct Pending
    {
        Plan plan;
        Global::Settings settings;
        std::vector<Signature> signatures;
        DirtyLayers dirty;
    };
    static bool Fail(std::string* error, const char* message) { if (error) *error = message; return false; }
    static bool SameSettings(const Global::Settings& a, const Global::Settings& b)
    {
        return a.cellsPerAxis == b.cellsPerAxis && a.clipmapCount == b.clipmapCount &&
            a.baseCellSize == b.baseCellSize && a.layerCellsPerAxis == b.layerCellsPerAxis;
    }
    static bool SameGeometry(const Signature& a, const Signature& b)
    {
        return a.asset == b.asset && std::memcmp(&a.worldFromLocal, &b.worldFromLocal, sizeof(a.worldFromLocal)) == 0 &&
            std::memcmp(&a.bounds, &b.bounds, sizeof(a.bounds)) == 0;
    }
    static void AddDirty(DirtyLayers& dirty, const Bounds& bounds)
    {
        for (auto& layer : dirty)
        {
            if (layer.full) continue;
            if (layer.bounds.size() >= MaximumRegionsPerLayer)
            {
                layer.bounds.clear(); layer.full = true;
            }
            else layer.bounds.push_back(bounds);
        }
    }
    static std::array<int64_t, 3> CellDelta(const Global::Clipmap& old, const Global::Clipmap& current)
    {
        return {int64_t(std::llround((double(current.origin.x) - old.origin.x) / current.cellSize)),
            int64_t(std::llround((double(current.origin.y) - old.origin.y) / current.cellSize)),
            int64_t(std::llround((double(current.origin.z) - old.origin.z) / current.cellSize))};
    }
    static uint32_t Mod(int64_t value, uint32_t modulus)
    {
        const auto result = value % int64_t(modulus);
        return uint32_t(result < 0 ? result + modulus : result);
    }
    static uint64_t Count(const Region& region) { return uint64_t(region.size[0]) * region.size[1] * region.size[2]; }
    static bool Contains(const Global::Clipmap& outer, const Global::Clipmap& inner)
    {
        return outer.origin.x <= inner.origin.x && outer.origin.y <= inner.origin.y && outer.origin.z <= inner.origin.z &&
            outer.boundsMaximum.x >= inner.boundsMaximum.x && outer.boundsMaximum.y >= inner.boundsMaximum.y &&
            outer.boundsMaximum.z >= inner.boundsMaximum.z;
    }
    static void AppendFull(Plan& plan, uint32_t layer)
    {
        const auto n = Global::Resolution(plan.layout.constants, layer);
        plan.regions.push_back({{0, 0, 0}, layer, {n, n, n}, 0});
        plan.fullUpdate[layer] = true;
        plan.layout.constants.clipmaps[layer].scrollOffset = {};
    }
    static void AppendSlabs(Plan& plan, uint32_t layer, const std::array<int64_t, 3>& delta)
    {
        const uint32_t n = Global::Resolution(plan.layout.constants, layer);
        std::array<uint32_t, 3> keptMinimum = {}, keptSize = {n, n, n};
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            if (!delta[axis]) continue;
            Region region{keptMinimum, layer, keptSize, 0};
            region.minimum[axis] = delta[axis] > 0 ? n - uint32_t(delta[axis]) : 0;
            region.size[axis] = uint32_t(std::abs(delta[axis]));
            plan.regions.push_back(region);
            keptMinimum[axis] = delta[axis] < 0 ? uint32_t(-delta[axis]) : 0;
            keptSize[axis] = n - uint32_t(std::abs(delta[axis]));
        }
    }
    static void AppendBounds(Plan& plan, uint32_t layer, const Bounds& bounds)
    {
        const auto& clipmap = plan.layout.constants.clipmaps[layer];
        const uint32_t n = Global::Resolution(plan.layout.constants, layer);
        const float origin[] = {clipmap.origin.x, clipmap.origin.y, clipmap.origin.z};
        const float minimum[] = {bounds.minimum.x, bounds.minimum.y, bounds.minimum.z};
        const float maximum[] = {bounds.maximum.x, bounds.maximum.y, bounds.maximum.z};
        Region region{{}, layer, {}, 0};
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            const double magnitude = (std::max)({std::abs(double(minimum[axis])), std::abs(double(maximum[axis])), std::abs(double(origin[axis]))});
            const double margin = double(clipmap.cellSize) * 1e-4 + magnitude * std::numeric_limits<float>::epsilon() * 8;
            const double low = std::floor((double(minimum[axis]) - clipmap.distanceLimit - margin - origin[axis]) / clipmap.cellSize);
            const double high = std::ceil((double(maximum[axis]) + clipmap.distanceLimit + margin - origin[axis]) / clipmap.cellSize);
            if (high < 0 || low > n - 1) return;
            const uint32_t first = uint32_t((std::max)(0.0, low));
            const uint32_t last = uint32_t((std::min)(double(n - 1), high));
            region.minimum[axis] = first;
            region.size[axis] = last - first + 1;
        }
        plan.regions.push_back(region);
    }

    Layout m_layout;
    Global::Settings m_settings;
    std::vector<Signature> m_signatures;
    DirtyLayers m_dirty;
    Pending m_pending;
    bool m_initialized = false, m_hasPending = false;
};
