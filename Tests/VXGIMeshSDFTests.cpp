#include "../Resources/VXGIMeshSDF.h"
#include <iostream>
#include <memory>
#include <stdexcept>

namespace
{
    struct Vertex
    {
        DirectX::XMFLOAT3 Position = {};
        DirectX::XMFLOAT3 Normal = { 0, 1, 0 };
        DirectX::XMFLOAT2 TexCoords = {};
        DirectX::XMFLOAT3 Tangent = { 1, 0, 0 };
        DirectX::XMFLOAT3 Bitangent = { 0, 0, 1 };
    };
    struct Mesh
    {
        std::vector<Vertex> vertices;
        std::vector<std::vector<unsigned>> lodIndices = { {} };
        uint32_t materialID = 7;
        bool isUnlit = false;
    };
    struct Model { std::vector<Mesh> meshes; };
    struct Instance
    {
        Model* pModel = nullptr;
        DirectX::XMFLOAT3 translation = {}, rotation = {}, scale = { 1, 1, 1 };
        uint32_t editorId = 0, customMaterialID = UINT32_MAX;
        bool isTransparent = false, isCutout = false, isVisible = true;
        int currentLodLevel = 0;
    };
    using Settings = VXGIMeshSDFCache::BuildSettings;

    void Check(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    bool Near(double a, double b, double tolerance = 1e-5)
    {
        return std::abs(a - b) <= tolerance;
    }
    bool Same(DirectX::XMFLOAT3 a, DirectX::XMFLOAT3 b)
    {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
    bool Same(DirectX::XMFLOAT2 a, DirectX::XMFLOAT2 b)
    {
        return a.x == b.x && a.y == b.y;
    }
    void Quad(Mesh& mesh, DirectX::XMFLOAT3 a, DirectX::XMFLOAT3 b,
        DirectX::XMFLOAT3 c, DirectX::XMFLOAT3 d)
    {
        const auto first = static_cast<unsigned>(mesh.vertices.size());
        mesh.vertices.insert(mesh.vertices.end(), { { a }, { b }, { c }, { d } });
        mesh.lodIndices[0].insert(mesh.lodIndices[0].end(),
            { first, first + 1, first + 2, first, first + 2, first + 3 });
    }
    Model LocalBox()
    {
        // Deliberately off origin and split over two material submeshes.
        Mesh first, second;
        first.materialID = 13;
        second.materialID = 29;
        Quad(first, { 1, -2, 6 }, { 3, -2, 6 }, { 3, 1, 6 }, { 1, 1, 6 });
        Quad(first, { 1, -2, 4 }, { 1, 1, 4 }, { 3, 1, 4 }, { 3, -2, 4 });
        Quad(first, { 3, -2, 4 }, { 3, 1, 4 }, { 3, 1, 6 }, { 3, -2, 6 });
        Quad(second, { 1, -2, 4 }, { 1, -2, 6 }, { 1, 1, 6 }, { 1, 1, 4 });
        Quad(second, { 1, 1, 4 }, { 1, 1, 6 }, { 3, 1, 6 }, { 3, 1, 4 });
        Quad(second, { 1, -2, 4 }, { 3, -2, 4 }, { 3, -2, 6 }, { 1, -2, 6 });
        return { { std::move(first), std::move(second) } };
    }
    void CheckNeutralIdentity(const VXGISDF& sdf)
    {
        for (const auto& triangle : sdf.GetTriangles())
            Check(triangle.instanceId == 0 && triangle.sourceInstanceIndex == UINT32_MAX &&
                triangle.materialId == UINT32_MAX && !triangle.isCutout,
                "asset geometry contains instance or material bindings");
    }
    double BoxDistance(DirectX::XMFLOAT3 p)
    {
        const double dx = (std::max)({ 1.0 - p.x, double(p.x) - 3, 0.0 });
        const double dy = (std::max)({ -2.0 - p.y, double(p.y) - 1, 0.0 });
        const double dz = (std::max)({ 4.0 - p.z, double(p.z) - 6, 0.0 });
        if (dx || dy || dz) return std::sqrt(dx * dx + dy * dy + dz * dz);
        return (std::min)({ double(p.x) - 1, 3.0 - p.x, double(p.y) + 2,
            1.0 - p.y, double(p.z) - 4, 6.0 - p.z });
    }
    void TestLocalGeometryAndDistances()
    {
        auto model = LocalBox();
        VXGIMeshSDFCache cache;
        const Settings settings = { 0.5f, 128 };
        const auto asset = cache.GetOrCreate(model, settings);
        Check(bool(asset), cache.GetLastError().c_str());
        const auto& sdf = asset->GetSDF();
        Check(asset->GetStats().meshes == 2 && asset->GetStats().sourceTriangles == 12 &&
            sdf.GetStats().triangles == 12, "multiple submeshes were not collected into one asset");
        CheckNeutralIdentity(sdf);
        for (const DirectX::XMFLOAT3 p : { DirectX::XMFLOAT3{ 2, -0.5f, 5 },
            { 0, -3, 3 }, { 3, 1, 6 }, { 2.5f, 0, 5.25f } })
            Check(Near(sdf.NearestDistance(p), BoxDistance(p)), "model-local box distance is incorrect");
        const auto& grid = sdf.GetGrid();
        Check(grid.valid && grid.origin.x == 0.5f && grid.origin.y == -2.5f && grid.origin.z == 3.5f &&
            grid.boundsMaximum.x == 3.5f && grid.boundsMaximum.y == 1.5f && grid.boundsMaximum.z == 6.5f,
            "asset grid is not in the original model-local coordinates");
        const auto dimensions = grid.nodeDimensions;
        for (uint32_t z = 0; z < dimensions[2]; ++z)
            for (uint32_t y = 0; y < dimensions[1]; ++y)
                for (uint32_t x = 0; x < dimensions[0]; ++x)
                {
                    const DirectX::XMFLOAT3 p = { grid.origin.x + float(x) * grid.cellSize,
                        grid.origin.y + float(y) * grid.cellSize, grid.origin.z + float(z) * grid.cellSize };
                    const float stored = sdf.GetNodes()[x + dimensions[0] * (y + dimensions[1] * z)];
                    const double analytical = BoxDistance(p);
                    Check(stored <= analytical + 1e-12 &&
                        Near(stored, analytical, sdf.GetStats().maximumNodePositionMargin + 2e-6),
                        "local distance nodes do not conservatively match the analytical fixture");
                }
        const auto stats = cache.GetStats();
        Check(stats.assets == 1 && stats.builds == 1 && stats.cacheHits == 0 &&
            stats.triangles == 12 && stats.nodes == sdf.GetNodes().size() && stats.memoryBytes > 0,
            "cache accounting does not describe the retained local asset");
    }
    void TestLODZeroAndSurfaceSources()
    {
        Model model;
        for (uint32_t submesh = 0; submesh < 2; ++submesh)
        {
            Mesh mesh;
            const float offset = float(submesh) * 2;
            mesh.vertices = { { { offset, 0, 0 } }, { { offset + 1, 0, 0 } },
                { { offset, 1, 0 } }, { { offset + 1, 1, 0 } }, { { 100, 100, 100 } } };
            for (size_t i = 0; i < mesh.vertices.size(); ++i)
            {
                auto& vertex = mesh.vertices[i];
                const float v = float(i) + float(submesh) * 10;
                vertex.TexCoords = { v * 0.125f, 1 - v * 0.0625f };
                vertex.Normal = { v + 1, v + 2, v + 3 };
                vertex.Tangent = { v + 4, v + 5, v + 6 };
                vertex.Bitangent = { v + 7, v + 8, v + 9 };
            }
            mesh.lodIndices = { { 2, 0, 3, 0, 1, 3 }, { 0, 4, 1 } };
            model.meshes.push_back(std::move(mesh));
        }
        VXGIMeshSDFCache cache;
        const auto asset = cache.GetOrCreate(model, { 0.5f, 128 });
        Check(bool(asset), cache.GetLastError().c_str());
        const auto& triangles = asset->GetSDF().GetTriangles();
        Check(triangles.size() == 4 && asset->GetStats().sourceTriangles == 4,
            "asset extraction used a render LOD or unused vertices");
        Check(asset->GetSDF().GetGrid().boundsMaximum.z < 1,
            "LOD1-only vertex expanded the local SDF bounds");
        for (size_t i = 0; i < triangles.size(); ++i)
        {
            const auto& triangle = triangles[i];
            Check(triangle.meshIndex == i / 2 && triangle.sourceTriangleIndex == i % 2,
                "local triangle lost its source submesh or LOD0 triangle identity");
            const auto& mesh = model.meshes[triangle.meshIndex];
            for (size_t corner = 0; corner < 3; ++corner)
            {
                const auto sourceIndex = mesh.lodIndices[0][size_t(triangle.sourceTriangleIndex) * 3 + corner];
                const auto& source = mesh.vertices[sourceIndex];
                Check(triangle.sourceVertexIndices[corner] == sourceIndex &&
                    Same(triangle.positions[corner], source.Position) && Same(triangle.uv[corner], source.TexCoords) &&
                    Same(triangle.shadingNormals[corner], source.Normal) && Same(triangle.tangents[corner], source.Tangent) &&
                    Same(triangle.bitangents[corner], source.Bitangent),
                    "local asset altered or detached the imported UV/TBN/source vertex attributes");
            }
        }
        CheckNeutralIdentity(asset->GetSDF());
    }
    void TestSharingAndInstanceIndependence()
    {
        auto model = LocalBox();
        Instance transparent;
        transparent.pModel = &model;
        transparent.isTransparent = true;
        transparent.translation = { 1000, -500, 300 };
        transparent.rotation = { 0.4f, -0.8f, 1.7f };
        transparent.scale = { -2, 0.3f, 4 };
        transparent.currentLodLevel = 8;
        transparent.customMaterialID = 88;
        transparent.isVisible = false;
        Instance cutout = transparent;
        cutout.editorId = 77;
        cutout.isTransparent = false;
        cutout.isCutout = true;
        cutout.customMaterialID = 99;
        cutout.translation = { -30, 200, -7 };
        VXGIMeshSDFCache cache;
        const Settings settings = { 0.5f, 128 };
        // The cache accepts only the shared model asset; instance flags and TRS
        // stay outside extraction and outside the cache key.
        const auto first = cache.GetOrCreate(*transparent.pModel, settings);
        const auto second = cache.GetOrCreate(*cutout.pModel, settings);
        Check(first && first == second && first->GetSDF().GetTriangles().size() == 12,
            "instances of the same model failed to share complete local geometry");
        const auto* nodes = first->GetSDF().GetNodes().data();
        const auto revision = first->GetSDF().GetRevision();
        model.meshes[0].materialID = 401;
        model.meshes[1].isUnlit = true;
        cutout.scale = { 1, 1, 1 };
        cutout.rotation = {};
        cutout.translation = {};
        cutout.isCutout = false;
        cutout.isVisible = true;
        cutout.currentLodLevel = 0;
        const auto third = cache.GetOrCreate(*cutout.pModel, settings);
        Check(third == first && third->GetSDF().GetNodes().data() == nodes &&
            third->GetSDF().GetRevision() == revision && cache.GetStats().assets == 1 &&
            cache.GetStats().builds == 1 && cache.GetStats().cacheHits == 2,
            "instance or material-only changes rebuilt the local asset");
        CheckNeutralIdentity(third->GetSDF());
        VXGIMeshSDFCache reverseCache;
        cutout.isCutout = true;
        const auto cutoutFirst = reverseCache.GetOrCreate(*cutout.pModel, settings);
        const auto transparentSecond = reverseCache.GetOrCreate(*transparent.pModel, settings);
        Check(cutoutFirst && cutoutFirst == transparentSecond &&
            cutoutFirst->GetSDF().GetNodes() == first->GetSDF().GetNodes(),
            "asset contents depend on the first requesting instance's coverage mode");
        CheckNeutralIdentity(cutoutFirst->GetSDF());
    }
    void TestSettingsKeysAndFailedBuilds()
    {
        auto model = LocalBox(), otherModel = LocalBox();
        VXGIMeshSDFCache cache;
        const Settings settings = { 0.5f, 128 };
        const auto original = cache.GetOrCreate(model, settings);
        const auto differentOwner = cache.GetOrCreate(otherModel, settings);
        const auto differentCell = cache.GetOrCreate(model, { 0.25f, 128 });
        const auto differentLimit = cache.GetOrCreate(model, { 0.5f, 64 });
        Check(original && differentOwner && differentCell && differentLimit &&
            original != differentOwner && original != differentCell && original != differentLimit,
            "model owner or build settings were omitted from the cache key");
        Check(differentCell->GetSDF().GetNodes().size() > original->GetSDF().GetNodes().size() &&
            differentCell->GetSettings().cellSize == 0.25f &&
            differentLimit->GetSettings().maxAxisResolution == 64,
            "asset did not retain or apply its build settings");
        const auto assets = cache.GetStats().assets, builds = cache.GetStats().builds;
        const auto* nodes = original->GetSDF().GetNodes().data();
        for (const Settings invalid : { Settings{ 0, 128 }, { -1, 128 },
            { std::numeric_limits<float>::infinity(), 128 },
            { std::numeric_limits<float>::quiet_NaN(), 128 }, { 0.5f, 2 }, { 0.5f, 257 }, { 0.01f, 3 } })
        {
            Check(!cache.GetOrCreate(model, invalid) && !cache.GetLastError().empty(),
                "invalid or over-budget local field did not fail with a diagnostic");
            Check(cache.GetStats().assets == assets && cache.GetStats().builds == builds,
                "failed local build polluted the asset cache");
        }
        const auto recovered = cache.GetOrCreate(model, settings);
        Check(recovered == original && original->GetSDF().GetNodes().data() == nodes &&
            cache.GetLastError().empty(), "failed build changed the valid cache entry or left a stale error");
    }
    void TestAutomaticDimensionsAndNodeBudget()
    {
        Check(Settings{}.maxAxisResolution == 0, "local asset builds still default to a manual axis resolution");
        Mesh mesh;
        // Binary-exact size and spacing make the padded dimensions unambiguous.
        // A thin asset needs more than 256 nodes on one axis, but only 6045 total.
        Quad(mesh, { -25, 0, -0.125f }, { -25, 0, 0.125f },
            { 25, 0, 0.125f }, { 25, 0, -0.125f });
        Model model = { { std::move(mesh) } };
        VXGIMeshSDFCache cache;
        Settings settings;
        settings.cellSize = 0.125f;
        const auto asset = cache.GetOrCreate(model, settings);
        Check(bool(asset), cache.GetLastError().c_str());
        const auto& grid = asset->GetSDF().GetGrid();
        Check(grid.cellSize == 0.125f && asset->GetSettings().cellSize == 0.125f &&
            asset->GetSettings().maxAxisResolution == 0,
            "automatic dimensions silently coarsened the requested cell spacing");
        Check(grid.nodeDimensions == std::array<uint32_t, 3>{403, 3, 5} &&
            grid.cellDimensions == std::array<uint32_t, 3>{402, 2, 4} &&
            Same(grid.origin, {-25.125f, -0.125f, -0.25f}) &&
            Same(grid.boundsMaximum, {25.125f, 0.125f, 0.25f}) &&
            asset->GetSDF().GetNodes().size() == 6045,
            "automatic dimensions failed to derive the padded grid from real local geometry");
        Check(std::all_of(asset->GetSDF().GetNodes().begin(), asset->GetSDF().GetNodes().end(),
            [](float value) { return std::isfinite(value) && value >= 0; }),
            "automatic long-axis field contains invalid unsigned distances");
        const auto* nodes = asset->GetSDF().GetNodes().data();
        const auto originalDistances = asset->GetSDF().GetNodes();
        const auto assets = cache.GetStats().assets, builds = cache.GetStats().builds;
        const auto cached = cache.GetOrCreate(model, settings);
        Check(cached == asset && cache.GetStats().builds == builds,
            "unchanged automatic build settings did not reuse the local asset");
        Settings tooFine;
        tooFine.cellSize = 0.0001f;
        Check(!cache.GetOrCreate(model, tooFine) &&
            cache.GetLastError().find("total grid budget of 16777216 nodes") != std::string::npos,
            "automatic dimensions accepted a field above the total node budget");
        Check(cache.GetStats().assets == assets && cache.GetStats().builds == builds &&
            asset->GetSDF().GetNodes().data() == nodes && asset->GetSDF().GetNodes() == originalDistances,
            "over-budget automatic build replaced or corrupted the previously cached asset");
        Check(cache.GetOrCreate(model, settings) == asset && cache.GetLastError().empty(),
            "valid automatic asset was not reusable after a rejected fine field");
    }
    void TestEmptyMalformedAndMissingAttributes()
    {
        struct MinimalVertex { DirectX::XMFLOAT3 Position; };
        struct MinimalMesh
        {
            std::vector<MinimalVertex> vertices;
            std::vector<std::vector<unsigned>> lodIndices;
        };
        struct MinimalModel { std::vector<MinimalMesh> meshes; };
        VXGIMeshSDFCache cache;
        MinimalModel empty;
        const auto emptyAsset = cache.GetOrCreate(empty, { 0.5f, 128 });
        Check(emptyAsset && !emptyAsset->GetSDF().GetGrid().valid &&
            emptyAsset->GetSDF().GetNodes().empty() && emptyAsset->GetSDF().GetTriangles().empty(),
            "empty model did not produce a reusable empty local asset");
        MinimalMesh mesh;
        mesh.vertices = { { { 0, 0, 0 } }, { { 1, 0, 0 } }, { { 0, 1, 0 } },
            { { std::numeric_limits<float>::quiet_NaN(), 0, 0 } } };
        mesh.lodIndices = { { 0, 1, 2, 0, 0, 0, 0, 1, 99, 0, 1, 3, 0 } };
        MinimalModel malformed = { { {}, std::move(mesh) } };
        const auto asset = cache.GetOrCreate(malformed, { 0.5f, 128 });
        Check(bool(asset), cache.GetLastError().c_str());
        Check(asset->GetStats().meshes == 2 && asset->GetStats().sourceTriangles == 4 &&
            asset->GetStats().invalidTriangles == 2 && asset->GetStats().degenerateTriangles == 1 &&
            asset->GetSDF().GetTriangles().size() == 1, "malformed LOD0 triangle accounting is incorrect");
        const auto& triangle = asset->GetSDF().GetTriangles().front();
        Check(triangle.meshIndex == 1 && triangle.sourceTriangleIndex == 0 &&
            Same(triangle.geometricNormal, { 0, 0, 1 }), "skipped submesh changed source geometry identity");
        for (size_t corner = 0; corner < 3; ++corner)
            Check(Same(triangle.shadingNormals[corner], triangle.geometricNormal),
                "geometry without imported TBN did not receive geometric shading normals");
        CheckNeutralIdentity(asset->GetSDF());
    }
    void TestConservativeGeometryBounds()
    {
        auto model = LocalBox();
        VXGIMeshSDFCache cache;
        const auto asset = cache.GetOrCreate(model, {1, 128});
        Check(bool(asset), cache.GetLastError().c_str());
        const auto& bounds = asset->GetTraceBounds();
        const auto& grid = asset->GetSDF().GetGrid();
        Check(bounds.valid && bounds.minimum.x > grid.origin.x && bounds.maximum.x < grid.boundsMaximum.x &&
            bounds.minimum.y > grid.origin.y && bounds.maximum.y < grid.boundsMaximum.y &&
            bounds.minimum.z > grid.origin.z && bounds.maximum.z < grid.boundsMaximum.z,
            "trace bounds still include the empty distance-grid padding");
        Check(bounds.minimum.x < 1 && bounds.maximum.x > 3 && bounds.minimum.y < -2 &&
            bounds.maximum.y > 1 && bounds.minimum.z < 4 && bounds.maximum.z > 6,
            "trace bounds truncated the exact local geometry");
        for (const auto& triangle : asset->GetSDF().GetTriangles())
            for (const std::array<double, 3> bary : {std::array<double, 3>{1.00004, -0.00002, -0.00002},
                {-0.00002, 1.00004, -0.00002}, {-0.00002, -0.00002, 1.00004}})
                for (size_t axis = 0; axis < 3; ++axis)
                {
                    const auto component = [axis](const auto& p) {return double(axis == 0 ? p.x : axis == 1 ? p.y : p.z);};
                    double coordinate = 0;
                    for (size_t i = 0; i < 3; ++i) coordinate += component(triangle.positions[i]) * bary[i];
                    Check(coordinate >= component(bounds.minimum) && coordinate <= component(bounds.maximum),
                        "trace bounds rejected the supported barycentric fringe");
                }
        Model empty;
        const auto emptyAsset = cache.GetOrCreate(empty, {1, 128});
        Check(emptyAsset && !emptyAsset->GetTraceBounds().valid, "empty asset acquired valid trace geometry bounds");
    }
    void TestSharedAssetLifetimeAndOwnedSnapshot()
    {
        auto model = LocalBox();
        const auto retained = [&model]
        {
            VXGIMeshSDFCache cache;
            auto asset = cache.GetOrCreate(model, { 0.5f, 128 });
            Check(bool(asset), cache.GetLastError().c_str());
            return asset;
        }();
        const auto source = retained->GetSDF().GetTriangles().front();
        const auto distances = retained->GetSDF().GetNodes();
        // There is no dynamic-geometry update request here. The cache is already
        // gone, and the returned immutable asset must own its CPU data.
        model.meshes[0].vertices[0].Position = { 900, 900, 900 };
        model.meshes.clear();
        Check(retained->GetSDF().GetTriangles().size() == 12 &&
            Same(retained->GetSDF().GetTriangles().front().positions[0], source.positions[0]) &&
            retained->GetSDF().GetNodes() == distances &&
            Near(retained->GetSDF().NearestDistance({ 2, -0.5f, 5 }), 1),
            "returned shared asset depends on cache lifetime or source mesh storage");
    }
}

int main()
{
    try
    {
        TestLocalGeometryAndDistances();
        TestLODZeroAndSurfaceSources();
        TestSharingAndInstanceIndependence();
        TestSettingsKeysAndFailedBuilds();
        TestAutomaticDimensionsAndNodeBudget();
        TestEmptyMalformedAndMissingAttributes();
        TestConservativeGeometryBounds();
        TestSharedAssetLifetimeAndOwnedSnapshot();
        std::cout << "VXGIMeshSDF: all 8 CPU test groups passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "VXGIMeshSDF: " << error.what() << '\n';
        return 1;
    }
}
