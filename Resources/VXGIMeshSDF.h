#pragma once

#include "VXGISDF.h"
#include <memory>

// Process-local cache for immutable model geometry. Own alongside the model
// resource owner: source model addresses must stay valid for this cache's life.
// No instance transform, material override, visibility or render LOD is baked in.
class VXGIMeshSDFCache
{
public:
    struct BuildSettings
    {
        float cellSize = 1.0f;
        // Production derives dimensions from geometry and cell size. Explicit
        // axis caps remain available only for internal numerical fixtures.
        uint32_t maxAxisResolution = 0;
        bool operator==(const BuildSettings&) const = default;
    };

    struct AssetStats
    {
        uint64_t meshes = 0;
        uint64_t sourceTriangles = 0;
        uint64_t invalidTriangles = 0;
        uint64_t degenerateTriangles = 0;
        double buildMilliseconds = 0;
    };

    struct TraceBounds
    {
        DirectX::XMFLOAT3 minimum = {}, maximum = {};
        bool valid = false;
    };

    class Asset
    {
    public:
        const VXGISDF& GetSDF() const { return m_sdf; }
        const BuildSettings& GetSettings() const { return m_settings; }
        const AssetStats& GetStats() const { return m_stats; }
        const TraceBounds& GetTraceBounds() const { return m_traceBounds; }

    private:
        friend class VXGIMeshSDFCache;
        VXGISDF m_sdf;
        BuildSettings m_settings;
        AssetStats m_stats;
        TraceBounds m_traceBounds;
    };

    struct Stats
    {
        uint64_t assets = 0;
        uint64_t builds = 0;
        uint64_t cacheHits = 0;
        uint64_t triangles = 0;
        uint64_t nodes = 0;
        uint64_t memoryBytes = 0; // Retained SDF buffers, excluding owner bookkeeping.
    };

    // Model has the existing meshes/vertices/lodIndices contract. The only
    // geometry input is the immutable model; instances cannot contaminate it.
    template<class ModelType>
    std::shared_ptr<const Asset> GetOrCreate(const ModelType& model, const BuildSettings& settings)
    {
        for (const auto& entry : m_entries)
        {
            if (entry.model == &model && entry.asset->GetSettings() == settings)
            {
                ++m_stats.cacheHits;
                m_error.clear();
                return entry.asset;
            }
        }
        const auto start = std::chrono::steady_clock::now();
        try
        {
            auto asset = std::make_shared<Asset>();
            asset->m_settings = settings;
            if (!asset->m_sdf.ValidateBuildSettings(settings.cellSize, settings.maxAxisResolution))
            {
                m_error = asset->m_sdf.GetLastError();
                return {};
            }
            std::vector<VXGIWorld::Triangle> triangles;
            ExtractTriangles(model, triangles, asset->m_stats);
            if (!asset->m_sdf.InitializeStaticMesh(std::move(triangles), settings.cellSize,
                settings.maxAxisResolution))
            {
                m_error = asset->m_sdf.GetLastError();
                return {};
            }
            asset->m_traceBounds = BuildTraceBounds(asset->m_sdf);
            asset->m_stats.buildMilliseconds = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            m_entries.push_back({&model, asset});
            ++m_stats.assets;
            ++m_stats.builds;
            m_stats.triangles += asset->GetSDF().GetStats().triangles;
            m_stats.nodes += asset->GetSDF().GetStats().nodes;
            m_stats.memoryBytes += asset->GetSDF().GetStats().memoryBytes;
            m_error.clear();
            return asset;
        }
        catch (const std::exception& exception)
        {
            m_error = std::string("Mesh SDF cache build failed: ") + exception.what();
            return {};
        }
    }

    const Stats& GetStats() const { return m_stats; }
    const std::string& GetLastError() const { return m_error; }

private:
    struct Entry
    {
        const void* model;
        std::shared_ptr<const Asset> asset;
    };
    std::vector<Entry> m_entries;
    Stats m_stats;
    std::string m_error;

    static bool Finite(DirectX::XMFLOAT3 value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    static TraceBounds BuildTraceBounds(const VXGISDF& sdf)
    {
        TraceBounds result;
        if (sdf.GetTriangles().empty()) return result;
        double minimum[3] = {INFINITY, INFINITY, INFINITY};
        double maximum[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (const auto& triangle : sdf.GetTriangles())
            for (size_t axis = 0; axis < 3; ++axis)
            {
                const auto component = [axis](const auto& p) { return double(axis == 0 ? p.x : axis == 1 ? p.y : p.z); };
                const double a = component(triangle.positions[0]), b = component(triangle.positions[1]), c = component(triangle.positions[2]);
                const double scale = (std::max)({std::abs(a), std::abs(b), std::abs(c)});
                // Match VXGITraceAcceleration::MakePrimitive: exact triangle
                // tests accept a barycentric fringe, which its bounds retain.
                const double margin = 4e-5 * (std::abs(b - a) + std::abs(c - a)) +
                    4 * std::numeric_limits<float>::epsilon() * scale + double(sdf.GetGrid().cellSize) * 1e-5;
                minimum[axis] = (std::min)(minimum[axis], (std::min)({a, b, c}) - margin);
                maximum[axis] = (std::max)(maximum[axis], (std::max)({a, b, c}) + margin);
            }
        const auto outward = [](double value, bool upper)
        {
            const float result = std::nextafter(static_cast<float>(value), upper ?
                std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity());
            if (!std::isfinite(result)) throw std::overflow_error("Mesh SDF trace bounds exceed finite float coordinates.");
            return result;
        };
        result.minimum = {outward(minimum[0], false), outward(minimum[1], false), outward(minimum[2], false)};
        result.maximum = {outward(maximum[0], true), outward(maximum[1], true), outward(maximum[2], true)};
        result.valid = true;
        return result;
    }

    template<class ModelType>
    static void ExtractTriangles(const ModelType& model, std::vector<VXGIWorld::Triangle>& triangles,
        AssetStats& stats)
    {
        stats.meshes = model.meshes.size();
        for (size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex)
        {
            const auto& mesh = model.meshes[meshIndex];
            if (mesh.lodIndices.empty()) continue;
            const auto& indices = mesh.lodIndices[0];
            stats.sourceTriangles += indices.size() / 3;
            for (size_t indexOffset = 0; indexOffset + 2 < indices.size(); indexOffset += 3)
            {
                VXGIWorld::Triangle triangle = {};
                triangle.meshIndex = static_cast<uint32_t>(meshIndex);
                triangle.sourceTriangleIndex = static_cast<uint32_t>(indexOffset / 3);
                triangle.sourceInstanceIndex = UINT32_MAX;
                triangle.materialId = UINT32_MAX; // Resolve mesh slot/instance override when tracing an instance.
                bool valid = true;
                for (size_t corner = 0; corner < 3; ++corner)
                {
                    const uint32_t vertexIndex = indices[indexOffset + corner];
                    triangle.sourceVertexIndices[corner] = vertexIndex;
                    if (vertexIndex >= mesh.vertices.size()) { valid = false; break; }
                    const auto& vertex = mesh.vertices[vertexIndex];
                    // Imported node transforms are already in the model vertices.
                    triangle.positions[corner] = vertex.Position;
                    if (!Finite(triangle.positions[corner])) { valid = false; break; }
                    if constexpr (requires { vertex.TexCoords; vertex.Normal; vertex.Tangent; vertex.Bitangent; })
                    {
                        triangle.uv[corner] = vertex.TexCoords;
                        triangle.shadingNormals[corner] = vertex.Normal;
                        triangle.tangents[corner] = vertex.Tangent;
                        triangle.bitangents[corner] = vertex.Bitangent;
                    }
                }
                if (!valid) { ++stats.invalidTriangles; continue; }
                const auto& a = triangle.positions[0];
                const auto& b = triangle.positions[1];
                const auto& c = triangle.positions[2];
                const double abx = double(b.x) - a.x, aby = double(b.y) - a.y, abz = double(b.z) - a.z;
                const double acx = double(c.x) - a.x, acy = double(c.y) - a.y, acz = double(c.z) - a.z;
                const double nx = aby * acz - abz * acy;
                const double ny = abz * acx - abx * acz;
                const double nz = abx * acy - aby * acx;
                const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (length == 0.0) { ++stats.degenerateTriangles; continue; }
                triangle.geometricNormal = {float(nx / length), float(ny / length), float(nz / length)};
                if constexpr (!requires { mesh.vertices[0].TexCoords; mesh.vertices[0].Normal;
                    mesh.vertices[0].Tangent; mesh.vertices[0].Bitangent; })
                    triangle.shadingNormals.fill(triangle.geometricNormal);
                triangles.push_back(triangle);
            }
        }
    }
};
