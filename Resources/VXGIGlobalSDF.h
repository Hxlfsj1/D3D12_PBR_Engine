#pragma once

#include "VXGIMeshSDFGPUData.h"
#include <array>
#include <cmath>
#include <limits>
#include <string>

// Global SDF contract. All distances in this project are unsigned.
// The GPU texture stores the clipmaps consecutively along Z. Each layer owns
// its boundary nodes; smaller layers use only their portion of the atlas XY.
class VXGIGlobalSDF
{
public:
    static constexpr uint32_t MaximumClipmaps = 3;
    static constexpr float InterpolationErrorCells = 0.8660254037844386f;

    struct Settings
    {
        uint32_t cellsPerAxis = 64, clipmapCount = 3;
        float baseCellSize = 0.5f;
        // Zero retains the shared legacy cell count for that layer.
        std::array<uint32_t, MaximumClipmaps> layerCellsPerAxis{};
    };
    struct Clipmap
    {
        DirectX::XMFLOAT3 origin; float cellSize;
        DirectX::XMFLOAT3 boundsMaximum; uint32_t nodeOffsetZ;
        std::array<uint32_t, 3> scrollOffset{}; float distanceLimit = 0;
    };
    struct Constants
    {
        uint32_t resolution = 0, clipmapCount = 0, assetCount = 0, nodeCount = 0;
        uint32_t instanceCount = 0;
        std::array<uint32_t, MaximumClipmaps> clipmapResolutions{};
        Clipmap clipmaps[MaximumClipmaps] = {};
    };
    struct Layout
    {
        Constants constants;
        uint32_t width = 0, height = 0, depth = 0;
        uint64_t totalNodeCount = 0;
        float maximumDistance = 0;
    };
    struct UpdateRegion
    {
        std::array<uint32_t, 3> minimum{}; uint32_t clipmapIndex = UINT32_MAX;
        std::array<uint32_t, 3> size{}; uint32_t padding = 0;
    };
    static_assert(sizeof(Clipmap) == 48 && offsetof(Clipmap, nodeOffsetZ) == 28 &&
        offsetof(Clipmap, scrollOffset) == 32);
    static_assert(sizeof(Constants) == 176 && offsetof(Constants, clipmaps) == 32 &&
        offsetof(Constants, clipmapResolutions) == 20);
    static_assert(sizeof(UpdateRegion) == 32 && offsetof(UpdateRegion, size) == 16);

    static uint32_t Resolution(const Constants& constants, uint32_t layer)
    {
        const uint32_t resolution = constants.clipmapResolutions[layer];
        return resolution ? resolution : constants.resolution;
    }

    // nodeCount describes the concatenated source Mesh SDF nodes, not the
    // output texture. Origins snap to whole cells, including negative centers.
    static bool BuildLayout(const DirectX::XMFLOAT3& center, const Settings& settings,
        uint32_t assetCount, uint32_t instanceCount, uint32_t sourceNodeCount,
        Layout& output, std::string* error = nullptr)
    {
        output = {};
        if (error) error->clear();
        if (!Finite(center) || !std::isfinite(settings.baseCellSize) || settings.baseCellSize <= 0)
            return Fail(error, "Global SDF center and cell size must be finite, with positive cell size.");
        if (settings.clipmapCount == 0 || settings.clipmapCount > MaximumClipmaps)
            return Fail(error, "Global SDF requires an even cell count between two and 256, and between one and three clipmaps.");
        Layout result;
        auto& constants = result.constants;
        uint64_t resolution = 0, depth = 0;
        for (uint32_t layer = 0; layer < settings.clipmapCount; ++layer)
        {
            const uint32_t cells = settings.layerCellsPerAxis[layer]
                ? settings.layerCellsPerAxis[layer] : settings.cellsPerAxis;
            if (cells < 2 || cells > 256 || (cells & 1))
                return Fail(error, "Global SDF requires even per-layer cell counts between two and 256.");
            constants.clipmapResolutions[layer] = cells + 1;
            constants.clipmaps[layer].nodeOffsetZ = uint32_t(depth);
            depth += cells + 1;
            resolution = (std::max)(resolution, uint64_t(cells) + 1);
        }
        // D3D12 Texture3D dimensions, plus 32-bit HLSL linear addressing.
        if (resolution > 2048 || depth > 2048 || resolution * resolution * depth > UINT32_MAX)
            return Fail(error, "Global SDF texture dimensions exceed the supported 3D texture layout.");
        result.width = result.height = uint32_t(resolution); result.depth = uint32_t(depth);
        result.totalNodeCount = resolution * resolution * depth;
        constants.resolution = uint32_t(resolution); constants.clipmapCount = settings.clipmapCount;
        constants.assetCount = assetCount; constants.instanceCount = instanceCount;
        constants.nodeCount = sourceNodeCount;
        for (uint32_t layer = 0; layer < settings.clipmapCount; ++layer)
        {
            auto& clipmap = constants.clipmaps[layer];
            clipmap.cellSize = std::ldexp(settings.baseCellSize, int(layer));
            const float span = float(Resolution(constants, layer) - 1) * clipmap.cellSize;
            if (!std::isfinite(span) || span <= 0)
                return Fail(error, "Global SDF clipmap span is not representable.");
            clipmap.origin = {SnapOrigin(center.x, clipmap.cellSize, span),
                SnapOrigin(center.y, clipmap.cellSize, span), SnapOrigin(center.z, clipmap.cellSize, span)};
            clipmap.boundsMaximum = {clipmap.origin.x + span, clipmap.origin.y + span, clipmap.origin.z + span};
            if (!Finite(clipmap.origin) || !Finite(clipmap.boundsMaximum) ||
                !(clipmap.origin.x + clipmap.cellSize > clipmap.origin.x) ||
                !(clipmap.origin.y + clipmap.cellSize > clipmap.origin.y) ||
                !(clipmap.origin.z + clipmap.cellSize > clipmap.origin.z))
                return Fail(error, "Global SDF grid cannot represent individual cells at this center.");
            if (layer)
            {
                const auto& inner = constants.clipmaps[layer - 1];
                if (clipmap.origin.x > inner.origin.x || clipmap.origin.y > inner.origin.y ||
                    clipmap.origin.z > inner.origin.z || clipmap.boundsMaximum.x < inner.boundsMaximum.x ||
                    clipmap.boundsMaximum.y < inner.boundsMaximum.y || clipmap.boundsMaximum.z < inner.boundsMaximum.z)
                    return Fail(error, "Global SDF clipmap world bounds must be nested.");
            }
            // A finite band makes geometric edits spatially local. Beyond this
            // radius all instances saturate to the same value, so old/new
            // world bounds expanded by the band cover every affected node.
            clipmap.distanceLimit = 4.0f * clipmap.cellSize;
            result.maximumDistance = Length({span, span, span});
            if (!std::isfinite(result.maximumDistance))
                return Fail(error, "Global SDF maximum distance is not representable.");
        }
        output = result;
        return true;
    }

    static bool ValidateSources(const VXGIMeshSDFGPUData::Geometry& geometry,
        const VXGIMeshSDFGPUData::Instances& sources, std::string* error = nullptr)
    {
        if (error) error->clear();
        if (geometry.assets.size() > UINT32_MAX || geometry.nodes.size() > UINT32_MAX || sources.instances.size() > UINT32_MAX)
            return Fail(error, "Global SDF source tables exceed 32-bit indexing.");
        for (const auto& asset : geometry.assets)
        {
            if (!Finite(asset.origin) || !Finite(asset.boundsMaximum) || !std::isfinite(asset.cellSize) || asset.cellSize <= 0)
                return Fail(error, "Global SDF source asset has invalid grid coordinates.");
            uint64_t nodeCount = 1;
            const float minimum[3] = {asset.origin.x, asset.origin.y, asset.origin.z};
            const float maximum[3] = {asset.boundsMaximum.x, asset.boundsMaximum.y, asset.boundsMaximum.z};
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                if (!asset.cellDimensions[axis] || asset.cellDimensions[axis] == UINT32_MAX)
                    return Fail(error, "Global SDF source asset must have a nonempty cell grid.");
                const uint64_t nodes = uint64_t(asset.cellDimensions[axis]) + 1;
                if (nodes > geometry.nodes.size() || nodeCount > geometry.nodes.size() / nodes)
                    return Fail(error, "Global SDF source asset node range is outside its buffer.");
                nodeCount *= nodes;
                const float expected = minimum[axis] + float(asset.cellDimensions[axis]) * asset.cellSize;
                const float tolerance = (std::max)(1e-5f, std::abs(expected) * 2e-6f);
                if (!std::isfinite(expected) || std::abs(maximum[axis] - expected) > tolerance)
                    return Fail(error, "Global SDF source bounds must describe its complete node lattice.");
            }
            if (uint64_t(asset.nodeOffset) + nodeCount > geometry.nodes.size())
                return Fail(error, "Global SDF source asset node range is outside its buffer.");
            for (uint64_t node = asset.nodeOffset; node < uint64_t(asset.nodeOffset) + nodeCount; ++node)
                if (!std::isfinite(geometry.nodes[size_t(node)]) || geometry.nodes[size_t(node)] < 0)
                    return Fail(error, "Global SDF requires finite nonnegative source distances.");
        }
        for (const auto& instance : sources.instances)
        {
            if (instance.assetIndex >= geometry.assets.size() || !ValidTRS(instance) ||
                !Finite(instance.boundsMinimum) || !Finite(instance.boundsMaximum) ||
                instance.boundsMaximum.x < instance.boundsMinimum.x ||
                instance.boundsMaximum.y < instance.boundsMinimum.y ||
                instance.boundsMaximum.z < instance.boundsMinimum.z)
                return Fail(error, "Global SDF source instance requires a valid asset and nonsingular TRS transform.");
        }
        return true;
    }

    // Reference composition of one world-grid node. ValidateSources is called
    // once before composing a field. Every instance participates; there is no
    // nearest-two AABB candidate limit. Invalid samples conservatively return 0.
    static float ComposeReferenceNode(const VXGIMeshSDFGPUData::Geometry& geometry,
        const VXGIMeshSDFGPUData::Instances& sources, const DirectX::XMFLOAT3& worldPosition,
        float maximumDistance)
    {
        if (!Finite(worldPosition) || !std::isfinite(maximumDistance) || maximumDistance <= 0) return 0;
        float result = maximumDistance;
        for (const auto& instance : sources.instances)
        {
            if (instance.assetIndex >= geometry.assets.size()) return 0;
            DirectX::XMFLOAT3 local;
            // Match mul(float4(position, 1), localFromWorld).xyz in HLSL;
            // the scene transforms are affine, with no homogeneous divide.
            DirectX::XMStoreFloat3(&local, DirectX::XMVector3Transform(DirectX::XMLoadFloat3(&worldPosition),
                DirectX::XMLoadFloat4x4(&instance.localFromWorld)));
            if (!Finite(local)) return 0;
            const auto& asset = geometry.assets[instance.assetIndex];
            const DirectX::XMFLOAT3 clamped = {Clamp(local.x, asset.origin.x, asset.boundsMaximum.x),
                Clamp(local.y, asset.origin.y, asset.boundsMaximum.y), Clamp(local.z, asset.origin.z, asset.boundsMaximum.z)};
            const float sampled = SampleLocal(geometry.nodes, asset, clamped);
            const float roundoff = (std::max)(asset.cellSize * 1e-4f, std::abs(sampled) * 2e-6f);
            const float lower = (std::max)(0.0f, sampled - InterpolationErrorCells * asset.cellSize - roundoff);
            const float extension = Length({local.x - clamped.x, local.y - clamped.y, local.z - clamped.z});
            const float localLower = (std::max)(extension, lower - extension);
            // The instance's conservative world bounds enclose its geometry.
            // This also limits a nonuniformly scaled instance's influence to
            // its world AABB plus the chosen band, rather than a stretched
            // local-space box that could affect arbitrarily distant nodes.
            const float boxDistance = Length({
                (std::max)({instance.boundsMinimum.x - worldPosition.x, 0.0f, worldPosition.x - instance.boundsMaximum.x}),
                (std::max)({instance.boundsMinimum.y - worldPosition.y, 0.0f, worldPosition.y - instance.boundsMaximum.y}),
                (std::max)({instance.boundsMinimum.z - worldPosition.z, 0.0f, worldPosition.z - instance.boundsMaximum.z})});
            const float worldLower = (std::max)(boxDistance, localLower * MinimumScale(instance.worldFromLocal));
            if (!std::isfinite(worldLower) || worldLower < 0) return 0;
            result = (std::min)(result, worldLower);
        }
        return result;
    }

    static DirectX::XMFLOAT3 NodePosition(const Clipmap& clipmap, uint32_t x, uint32_t y, uint32_t z)
    {
        return {clipmap.origin.x + float(x) * clipmap.cellSize,
            clipmap.origin.y + float(y) * clipmap.cellSize, clipmap.origin.z + float(z) * clipmap.cellSize};
    }

    static size_t PhysicalNodeIndex(const Layout& layout, uint32_t layer, uint32_t x, uint32_t y, uint32_t z)
    {
        const auto& clipmap = layout.constants.clipmaps[layer];
        const uint32_t resolution = Resolution(layout.constants, layer);
        const uint32_t px = (x + clipmap.scrollOffset[0]) % resolution;
        const uint32_t py = (y + clipmap.scrollOffset[1]) % resolution;
        const uint32_t pz = (z + clipmap.scrollOffset[2]) % resolution + clipmap.nodeOffsetZ;
        return px + size_t(layout.width) * (py + size_t(layout.height) * pz);
    }

private:
    static bool Fail(std::string* error, const char* message) { if (error) *error = message; return false; }
    static bool Finite(const DirectX::XMFLOAT3& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
    static float Clamp(float x, float a, float b) { return (std::max)(a, (std::min)(b, x)); }
    static float Length(const DirectX::XMFLOAT3& p) { return std::hypot(p.x, p.y, p.z); }
    static float SnapOrigin(float center, float cell, float span)
    {
        return float(std::floor(double(center) / cell) * cell - double(span) * 0.5);
    }
    static float MinimumScale(const DirectX::XMFLOAT4X4& m)
    {
        return (std::min)({Length({m._11, m._12, m._13}), Length({m._21, m._22, m._23}), Length({m._31, m._32, m._33})});
    }
    static bool ValidTRS(const VXGIMeshSDFGPUData::Instance& instance)
    {
        for (const auto* matrix : {&instance.worldFromLocal, &instance.localFromWorld})
        {
            for (const auto& row : matrix->m) for (float value : row) if (!std::isfinite(value)) return false;
            if (std::abs(matrix->_14) > 1e-6f || std::abs(matrix->_24) > 1e-6f ||
                std::abs(matrix->_34) > 1e-6f || std::abs(matrix->_44 - 1) > 1e-5f) return false;
        }
        const auto& m = instance.worldFromLocal;
        const DirectX::XMFLOAT3 basis[] = {{m._11,m._12,m._13}, {m._21,m._22,m._23}, {m._31,m._32,m._33}};
        float lengths[3] = {Length(basis[0]), Length(basis[1]), Length(basis[2])};
        for (float length : lengths) if (!std::isfinite(length) || length <= 0) return false;
        for (uint32_t a = 0; a < 3; ++a) for (uint32_t b = a + 1; b < 3; ++b)
        {
            const double dot = double(basis[a].x)*basis[b].x + double(basis[a].y)*basis[b].y + double(basis[a].z)*basis[b].z;
            if (std::abs(dot) > double(lengths[a])*lengths[b]*1e-4) return false;
        }
        return true;
    }
    static float SampleLocal(const std::vector<float>& nodes, const VXGIMeshSDFGPUData::Asset& asset,
        const DirectX::XMFLOAT3& p)
    {
        const float coordinates[] = {(p.x-asset.origin.x)/asset.cellSize,
            (p.y-asset.origin.y)/asset.cellSize, (p.z-asset.origin.z)/asset.cellSize};
        uint32_t base[3]; float weight[3];
        for (uint32_t axis=0; axis<3; ++axis)
        {
            const float coordinate=Clamp(coordinates[axis], 0, float(asset.cellDimensions[axis]));
            base[axis]=(std::min)(uint32_t(coordinate),asset.cellDimensions[axis]-1);
            weight[axis]=Clamp(coordinate-float(base[axis]),0,1);
        }
        const uint64_t nx=uint64_t(asset.cellDimensions[0])+1, ny=uint64_t(asset.cellDimensions[1])+1;
        float result=0;
        for(uint32_t z=0; z<2; ++z) for(uint32_t y=0; y<2; ++y) for(uint32_t x=0; x<2; ++x)
        {
            const uint64_t index=uint64_t(asset.nodeOffset)+(base[0]+x)+nx*((base[1]+y)+ny*(base[2]+z));
            if(index>=nodes.size()) return 0;
            result+=nodes[size_t(index)]*(x?weight[0]:1-weight[0])*(y?weight[1]:1-weight[1])*(z?weight[2]:1-weight[2]);
        }
        return result;
    }
};
