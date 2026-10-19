// Standalone CPU tests; no renderer/device, assets or asset-loader linkage required.
// Build from a VS Developer shell: cl /std:c++20 /EHsc /W4 /Fe:tmp\VXGIWorldTests.exe Tests\VXGIWorldTests.cpp
#include "../Resources/VXGIWorld.h"
#include "../Resources/VXGIProbeTopology.h"
#include "../Resources/VXGISDF.h"
#include <iostream>
#include <stdexcept>

namespace
{
    struct TestResource { void* Get() const { return nullptr; } };
    struct TestTexture { uint32_t type = 0; TestResource Resource; std::string path; };
    struct TestVertex { DirectX::XMFLOAT3 Position; };
    struct TestMesh
    {
        std::vector<TestVertex> vertices;
        std::vector<std::vector<unsigned>> lodIndices;
        std::vector<TestTexture> textures;
        uint32_t materialID = 7;
        bool isUnlit = false;
        DirectX::XMFLOAT4 baseColorFactor = {1,1,1,1};
        DirectX::XMFLOAT3 emissiveFactor = {};
    };
    struct TestModel { std::vector<TestMesh> meshes; };
    struct TestInstance
    {
        uint32_t editorId = 1;
        TestModel* pModel = nullptr;
        DirectX::XMFLOAT3 translation = {}, rotation = {}, scale = {1,1,1};
        uint32_t customMaterialID = UINT32_MAX;
        bool isTransparent = false, isCutout = false, isVisible = true, isDirty = true;
        int currentLodLevel = 0;
    };
    void Check(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    bool Near(float a, float b) { return std::abs(a-b) < 1e-5f; }
    TestModel MakeTriangle(std::array<DirectX::XMFLOAT3, 3> positions)
    {
        TestMesh mesh;
        for (auto position : positions) mesh.vertices.push_back({position});
        mesh.lodIndices = {{0,1,2}, {}, {}};
        return {{mesh}};
    }
    void TestTriangleBoxSAT()
    {
        const std::array<DirectX::XMFLOAT3, 3> triangle = {{{0.1f,0.1f,0.1f}, {3.9f,0.1f,0.1f}, {0.1f,3.9f,0.1f}}};
        Check(VXGIWorld::TriangleIntersectsBox(triangle, {0.5f,0.5f,0.5f}, {0.5f,0.5f,0.5f}), "triangle interior overlap rejected");
        Check(!VXGIWorld::TriangleIntersectsBox(triangle, {3.5f,3.5f,0.5f}, {0.5f,0.5f,0.5f}), "empty triangle AABB corner was occupied");
        Check(!VXGIWorld::TriangleIntersectsBox(triangle, {0.5f,0.5f,1.5f}, {0.5f,0.5f,0.5f}), "separated plane was occupied");
        const std::array<DirectX::XMFLOAT3, 3> touching = {{{1,0.2f,0.2f}, {1,0.8f,0.2f}, {1,0.2f,0.8f}}};
        Check(VXGIWorld::TriangleIntersectsBox(touching, {0.5f,0.5f,0.5f}, {0.5f,0.5f,0.5f}), "triangle touching box face rejected");
        // Diagonal edge cuts across the box without any triangle vertex inside it.
        const std::array<DirectX::XMFLOAT3, 3> crossing = {{{-2,0,0}, {2,0,0}, {0,2,0}}};
        Check(VXGIWorld::TriangleIntersectsBox(crossing, {0,0,0}, {0.1f,0.1f,0.1f}), "triangle crossing without contained vertices rejected");
        const std::array<DirectX::XMFLOAT3, 3> tiny = {{{1e-6f,1e-6f,0}, {9e-6f,1e-6f,0}, {1e-6f,9e-6f,0}}};
        Check(!VXGIWorld::TriangleIntersectsBox(tiny, {8e-6f,8e-6f,0}, {0.5e-6f,0.5e-6f,0.5e-6f}),
            "absolute SAT axis tolerance filled a tiny triangle's empty AABB corner");
    }
    void TestSparseAllocationAndCandidates()
    {
        auto model = MakeTriangle({{{0.1f,0.1f,0.1f}, {3.9f,0.1f,0.1f}, {0.1f,3.9f,0.1f}}});
        std::vector<TestInstance> scene = {{}}; scene[0].pModel = &model;
        VXGIWorld world;
        Check(world.Update(scene, 1), "basic allocation failed");
        Check(world.FindCell(VXGIWorld::Coordinate{0,0,0}) >= 0, "interior surface cell missing");
        Check(world.FindCell(VXGIWorld::Coordinate{3,3,0}) == -1, "triangle AABB was filled wholesale");
        Check(world.GetStats().cells < world.GetStats().candidateBoxTests, "SAT did not reject empty candidates");
        Check(world.GetStats().triangles == 1, "LOD0 triangle count wrong");
        for (size_t i = 0; i < world.GetCells().size(); ++i)
        {
            const auto& cell = world.GetCells()[i];
            Check(cell.probeIndex == i, "compact probe identity mismatch");
            Check(cell.triangleCount == 1 && world.GetTriangleReferences()[cell.firstTriangle] == 0, "surface candidate identity missing");
            Check(world.FindCell(world.GetOccupiedCenters()[i]) == int32_t(i), "position/index lookup mismatch");
            Check(world.GetProbes()[i].state == VXGIWorld::ProbeState::Unclassified, "stage 1 probe claimed classified lighting");
            for (const auto coefficient : world.GetProbes()[i].sh)
                Check(coefficient.x == 0 && coefficient.y == 0 && coefficient.z == 0, "stage 1 SH was nonzero");
            if (i) Check(world.GetCells()[i-1].coordinate < cell.coordinate, "cells are not deterministically sorted");
        }
        auto boundaryModel = MakeTriangle({{{1,0.2f,0.2f}, {1,0.8f,0.2f}, {1,0.2f,0.8f}}});
        scene[0].pModel = &boundaryModel;
        Check(world.Update(scene, 1), "boundary allocation failed");
        Check(world.GetStats().cells == 2, "exact boundary should allocate both adjacent closed cells");
        Check(world.FindCell(VXGIWorld::Coordinate{0,0,0}) >= 0 && world.FindCell(VXGIWorld::Coordinate{1,0,0}) >= 0,
            "one side of exact boundary missing");
    }
    void TestTransformsMaterialsAndInvalidation()
    {
        auto model = MakeTriangle({{{0.1f,0.1f,0.1f}, {0.9f,0.1f,0.1f}, {0.1f,0.9f,0.1f}}});
        std::vector<TestInstance> scene = {{}}; scene[0].pModel = &model;
        scene[0].translation = {2,3,4}; scene[0].scale = {2,3,1}; scene[0].rotation.z = DirectX::XM_PIDIV2;
        scene[0].customMaterialID = 42; scene[0].isDirty = false;
        VXGIWorld world;
        Check(world.Update(scene, 1), "transformed build failed");
        const auto triangle = world.GetTriangles()[0];
        Check(Near(triangle.positions[0].x, 1.7f) && Near(triangle.positions[0].y, 3.2f) && Near(triangle.positions[0].z, 4.1f),
            "nonuniform scale/rotation/translation incorrect");
        Check(Near(triangle.geometricNormal.z, 1), "transformed geometric normal wrong");
        Check(triangle.materialId == 42 && triangle.sourceVertexIndices[2] == 2, "override or source identity missing");
        const auto fingerprint = world.GetStats().geometryFingerprint;
        const auto revision = world.GetRevision();
        const auto cells = world.GetCells();
        scene[0].isVisible = false; scene[0].currentLodLevel = 2; scene[0].isDirty = true;
        Check(world.Update(scene, 1) && world.GetRevision() == revision, "camera/LOD/dirty flag rebuilt world allocation");
        Check(world.GetStats().geometryFingerprint == fingerprint && world.GetCells().size() == cells.size(), "camera changed allocation fingerprint");
        for (size_t i = 0; i < cells.size(); ++i)
            Check(world.GetCells()[i].coordinate == cells[i].coordinate, "camera changed cell identity");
        scene[0].translation.x += 2; scene[0].isDirty = false;
        Check(world.Update(scene, 1) && world.GetRevision() == revision+1, "transform change after dirty flag clear missed");
        Check(world.GetStats().geometryFingerprint != fingerprint, "world geometry fingerprint ignored transform");
        const auto movedRevision = world.GetRevision();
        model.meshes[0].baseColorFactor.x = 0.25f;
        Check(world.Update(scene, 1) && world.GetRevision() == movedRevision+1, "material-factor change missed");
        const auto materialRevision = world.GetRevision();
        scene[0].customMaterialID = 17;
        Check(world.Update(scene, 1) && world.GetRevision() == materialRevision+1 && world.GetTriangles()[0].materialId == 17,
            "custom material change missed");
        model.meshes[0].vertices[0].Position.x += 0.2f;
        world.Invalidate();
        Check(world.Update(scene, 1), "explicit vertex edit invalidation failed");
        Check(!Near(world.GetTriangles()[0].positions[0].y, triangle.positions[0].y), "explicit vertex edit not registered");
    }
    void TestCutoutTransparencyReorderAndFailure()
    {
        auto model = MakeTriangle({{{0.1f,0.1f,0.1f}, {0.9f,0.1f,0.1f}, {0.1f,0.9f,0.1f}}});
        std::vector<TestInstance> scene(2);
        scene[0].pModel = scene[1].pModel = &model;
        scene[0].editorId = 11; scene[0].isCutout = true;
        scene[1].editorId = 22; scene[1].isTransparent = true; scene[1].translation = {100,100,100};
        VXGIWorld world;
        Check(world.Update(scene, 1), "cutout conservative build failed");
        Check(world.GetStats().sceneInstances == 2 && world.GetStats().includedInstances == 1 &&
            world.GetStats().cutoutInstances == 1 && world.GetStats().transparentInstances == 1, "scope counts wrong");
        Check(world.GetTriangles()[0].isCutout && world.GetStats().triangles == 1, "cutout geometry lost or transparent included");
        const auto fingerprint = world.GetStats().geometryFingerprint;
        std::swap(scene[0], scene[1]);
        Check(world.Update(scene, 1) && world.GetStats().geometryFingerprint == fingerprint, "batch reorder changed geometry signature");
        Check(world.GetTriangles()[0].sourceInstanceIndex == 1, "batch reorder left stale source index");
        const auto revision = world.GetRevision();
        Check(!world.Update(scene, 0) && !world.GetLastError().empty() && world.GetRevision() == revision, "invalid voxel size mutated cache");
        scene[1].translation.x = std::numeric_limits<float>::infinity();
        Check(!world.Update(scene, 1) && world.GetRevision() == revision && world.GetStats().geometryFingerprint == fingerprint,
            "failed geometry build destroyed previous cache");
        scene[1].translation.x = 0;
        Check(world.Update(scene, 1) && world.GetLastError().empty(), "failure did not recover");
    }
    void TestEmptyInvalidAndBudget()
    {
        VXGIWorld world;
        std::vector<TestInstance> empty;
        Check(world.Update(empty, 1) && world.GetStats().cells == 0 && !world.GetGrid().valid, "empty scene failed");
        Check(world.FindCell(DirectX::XMFLOAT3{0,0,0}) == -1, "empty scene returned an allocated cell");
        auto model = MakeTriangle({{{0,0,0}, {1,0,0}, {2,0,0}}});
        model.meshes[0].lodIndices[0].insert(model.meshes[0].lodIndices[0].end(), {0,1,50});
        std::vector<TestInstance> scene = {{}}; scene[0].pModel = &model;
        Check(world.Update(scene, 1) && world.GetStats().degenerateTriangles == 1 && world.GetStats().invalidTriangles == 1 &&
            world.GetStats().cells == 0, "invalid/degenerate geometry handling failed");
        auto large = MakeTriangle({{{0,0,0}, {100,0,0}, {0,100,0}}});
        scene[0].pModel = &large;
        Check(!world.Update(scene, 0.001f) && world.GetLastError().find("triangle/cell tests") != std::string::npos,
            "pathological allocation budget was not checked");
    }
    void TestMirrorsSharedAssetsAndFractionalGrid()
    {
        auto model = MakeTriangle({{{0.1f,0.1f,0.1f}, {0.9f,0.1f,0.1f}, {0.1f,0.9f,0.1f}}});
        std::vector<TestInstance> scene(2);
        scene[0].pModel = scene[1].pModel = &model;
        scene[0].editorId = 11; scene[0].scale = {-2,3,1}; scene[0].translation = {3,0,0};
        scene[1].editorId = 22; scene[1].translation = {-4,0,0};
        VXGIWorld world;
        Check(world.Update(scene, 0.3f), "fractional-grid mirrored build failed");
        Check(world.GetStats().triangles == 2 && Near(world.GetTriangles()[0].geometricNormal.z, -1), "mirror winding was not preserved");
        const auto fingerprint = world.GetStats().geometryFingerprint;
        const auto cells = world.GetCells();
        std::swap(scene[0], scene[1]);
        Check(world.Update(scene, 0.3f) && world.GetStats().geometryFingerprint == fingerprint, "multi-instance reorder changed geometry signature");
        Check(world.GetTriangles()[0].instanceId == 11 && world.GetTriangles()[0].sourceInstanceIndex == 1,
            "multi-instance source identity was not refreshed");
        for (size_t i = 0; i < cells.size(); ++i)
            Check(cells[i].coordinate == world.GetCells()[i].coordinate, "multi-instance reorder changed compact cell IDs");
        model.meshes[0].emissiveFactor.x = 2;
        const auto revision = world.GetRevision();
        Check(world.Update(scene, 0.3f) && world.GetRevision() == revision+1, "shared-model metadata dedup missed material edit");
        // Large coordinate, non-binary cell width. The geometry calculation must
        // use exact double lattice centers, not float-rounded render positions.
        auto distant = MakeTriangle({{{10000,0.01f,0.01f}, {10000,0.02f,0.01f}, {10000,0.01f,0.02f}}});
        scene.resize(1); scene[0] = {}; scene[0].pModel = &distant;
        const float cellSize = 0.0003f;
        Check(world.Update(scene, cellSize) && world.GetStats().cells > 0, "float-rounded grid center caused distant surface to disappear");
        const auto expectedX = int32_t(std::floor(10000.0/double(cellSize)));
        bool correctColumn = false;
        for (const auto& cell : world.GetCells()) correctColumn |= cell.coordinate.x == expectedX;
        Check(correctColumn, "fractional grid lost actual geometry column");
        auto tiny = MakeTriangle({{{1e-6f,1e-6f,1e-6f}, {9e-6f,1e-6f,1e-6f}, {1e-6f,9e-6f,1e-6f}}});
        scene[0].pModel = &tiny;
        Check(world.Update(scene, 1e-5f) && world.GetStats().triangles == 1 && world.GetStats().degenerateTriangles == 0,
            "small but nondegenerate triangle was discarded");
        // Repeated updates inspect transforms and one copy of shared asset
        // metadata; they do not scan retained vertex/index contents or rebuild.
        scene.resize(1000);
        for (size_t i = 0; i < scene.size(); ++i) { scene[i] = {}; scene[i].editorId = uint32_t(i+1); scene[i].pModel = &model; }
        Check(world.Update(scene, 1), "shared asset stress build failed");
        const auto sharedRevision = world.GetRevision();
        const auto start = std::chrono::steady_clock::now();
        for (size_t iteration = 0; iteration < 100; ++iteration)
            Check(world.Update(scene, 1) && world.GetRevision() == sharedRevision, "unchanged shared assets rebuilt");
        const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count()/100;
        std::cout << "Unchanged Update, 1000 shared-model instances: " << milliseconds << " ms average.\n";
    }
    void TestProbePaddingAllocationAndTopology()
    {
        auto model = MakeTriangle({{{0.1f,0.1f,0.1f}, {0.9f,0.1f,0.1f}, {0.1f,0.9f,0.1f}}});
        std::vector<TestInstance> scene(1); scene[0].pModel = &model;
        VXGIWorld world;
        VXGIProbeTopology topology;
        VXGISDF sdf;
        Check(world.Update(scene,1,0),"zero-layer probe build failed");
        Check(topology.Update(world) && sdf.Update(world,0.5f,32),"zero-layer dependent caches failed");
        const auto geometry = world.GetCells();
        const auto references = world.GetTriangleReferences();
        const auto center = world.GetOccupiedCenters().front();
        const auto grid = world.GetGrid();
        const auto* triangleData = world.GetTriangles().data();
        const auto* cellData = world.GetCells().data();
        const auto* referenceData = world.GetTriangleReferences().data();
        const auto* distances = sdf.GetNodes().data();
        const auto nodes = sdf.GetNodes();
        const auto geometryRevision = world.GetGeometryRevision();
        const auto materialRevision = world.GetMaterialRevision();
        const auto coverageRevision = world.GetCoverageRevision();
        const auto fingerprint = world.GetStats().geometryFingerprint;
        const auto sdfGeometryRevision = sdf.GetGeometryRevision();
        auto allocationRevision = world.GetAllocationRevision();
        auto topologyRevision = topology.GetRevision();
        Check(geometry.size()==1 && world.GetProbeCells().size()==1,"single marker occupied more than its one cell");
        for (uint32_t layers : {1u,2u,0u})
        {
            Check(world.Update(scene,1,layers),"probe padding update failed");
            const uint64_t width=2*layers+1;
            Check(world.GetStats().cells==1 && world.GetStats().probeCells==width*width*width &&
                world.GetStats().haloProbeCells==width*width*width-1 && world.GetProbePaddingLayers()==layers,
                "padding count omitted face/edge/corner cells or changed geometry occupancy");
            Check(world.GetCells().data()==cellData && world.GetTriangles().data()==triangleData &&
                world.GetTriangleReferences().data()==referenceData && world.GetTriangleReferences()==references,
                "padding rebuilt geometry or changed its triangle CSR");
            Check(world.GetGeometryRevision()==geometryRevision && world.GetMaterialRevision()==materialRevision &&
                world.GetCoverageRevision()==coverageRevision && world.GetStats().geometryFingerprint==fingerprint &&
                world.GetAllocationRevision()==++allocationRevision && !world.GetStats().geometryRebuilt,
                "padding changed geometry/material/coverage identity or failed to revise allocation");
            Check(world.GetGrid().minimum==grid.minimum && world.GetGrid().maximum==grid.maximum,
                "probe halo expanded the geometry grid");
            const auto probeIndex=world.FindProbeCell(VXGIWorld::Coordinate{0,0,0});
            Check(probeIndex>=0,"original probe coordinate missing after padding");
            const auto original=world.GetOccupiedCenters()[size_t(probeIndex)];
            Check(original.x==center.x && original.y==center.y && original.z==center.z,
                "padding moved the original probe center");
            Check(topology.Update(world) && topology.GetRevision()==++topologyRevision,"topology missed padding-only change");
            const int32_t limit=static_cast<int32_t>(layers);
            for (int32_t x=-limit;x<=limit;++x)
                for (int32_t y=-limit;y<=limit;++y)
                    for (int32_t z=-limit;z<=limit;++z)
                    {
                        const VXGIWorld::Coordinate coordinate={x,y,z};
                        const auto found=world.FindProbeCell(coordinate);
                        Check(found>=0 && topology.FindProbe(coordinate)==uint32_t(found),"halo topology omitted a coordinate");
                        Check(world.FindCell(coordinate)==((x==0 && y==0 && z==0)?0:-1),
                            "probe halo created false geometry occupancy");
                    }
            for (size_t index=0;index<world.GetProbeCells().size();++index)
            {
                const auto& cell=world.GetProbeCells()[index];
                Check(cell.probeIndex==index && cell.firstTriangle==0 && cell.triangleCount==0,
                    "probe allocation retained geometry CSR references or wrong compact index");
                if(index) Check(world.GetProbeCells()[index-1].coordinate<cell.coordinate,"halo coordinates not unique and sorted");
                Check(world.FindProbeCell(world.GetOccupiedCenters()[index])==int32_t(index),"halo world-center lookup mismatch");
            }
            const auto expectedMinimum=VXGIWorld::Coordinate{-limit,-limit,-limit};
            const auto expectedMaximum=VXGIWorld::Coordinate{limit,limit,limit};
            Check(world.GetProbeGrid().minimum==expectedMinimum && world.GetProbeGrid().maximum==expectedMaximum,
                "probe grid omitted expanded bounds");
            if(layers)
            {
                std::array<VXGIProbeTopology::QueryCandidate,8> candidates;
                Check(topology.GetQueryCandidates({0.01f,0.01f,0.01f},candidates),"padded eight-corner query failed");
                for(const auto& candidate:candidates) Check(candidate.probeIndex!=VXGIProbeTopology::Missing,
                    "one layer did not supply all eight corners within original cell");
            }
            Check(sdf.Update(world,0.5f,32) && sdf.GetGeometryRevision()==sdfGeometryRevision &&
                sdf.GetNodes().data()==distances && sdf.GetNodes()==nodes,
                "padding changed distance-field data or forced a distance bake");
            const auto revision=world.GetRevision();
            const auto* probeData=world.GetProbeCells().data();
            const auto* centers=world.GetOccupiedCenters().data();
            Check(world.Update(scene,1,layers) && world.GetRevision()==revision &&
                world.GetProbeCells().data()==probeData && world.GetOccupiedCenters().data()==centers &&
                topology.Update(world) && topology.GetRevision()==topologyRevision,
                "unchanged padding bypassed the static fast path");
        }
        const auto revision=world.GetRevision();
        Check(!world.Update(scene,1,3) && world.GetRevision()==revision && world.GetStats().probeCells==1,
            "out-of-range padding changed committed allocation");

        // Adjacent source cells overlap most of their halo. Duplicate geometry
        // instances must neither duplicate probes nor make ordering depend on
        // the scene's batching order.
        scene.resize(3); scene[1]=scene[0]; scene[2]=scene[0];
        scene[1].editorId=2; scene[1].translation={1,0,0}; scene[2].editorId=3;
        for(uint32_t layers:{1u,2u})
        {
            Check(world.Update(scene,1,layers),"overlapping probe halo build failed");
            const uint64_t width=2*layers+1;
            Check(world.GetStats().cells==2 && world.GetProbeCells().size()==(width+1)*width*width,
                "overlapping halo did not deduplicate coordinate identity");
            const auto cells=world.GetProbeCells();
            std::swap(scene[0],scene[1]);
            Check(world.Update(scene,1,layers) && world.GetProbeCells().size()==cells.size(),"batch reorder altered halo count");
            for(size_t index=0;index<cells.size();++index)
                Check(cells[index].coordinate==world.GetProbeCells()[index].coordinate,
                    "batch reorder changed deterministic probe coordinates");
        }
        Check(world.Update(std::vector<TestInstance>{},1,2) && world.GetProbeCells().empty() &&
            world.GetOccupiedCenters().empty() && !world.GetProbeGrid().valid && world.GetStats().haloProbeCells==0,
            "empty scene retained a probe halo");
        std::cout << "Probe padding: 0/1/2 full neighborhoods, overlap, topology, immutable geometry/SDF and revision reuse passed.\n";
    }
}

int main()
{
    try
    {
        TestTriangleBoxSAT(); TestSparseAllocationAndCandidates(); TestTransformsMaterialsAndInvalidation();
        TestCutoutTransparencyReorderAndFailure(); TestEmptyInvalidAndBudget(); TestMirrorsSharedAssetsAndFractionalGrid();
        TestProbePaddingAllocationAndTopology();
        std::cout << "VXGIWorld: all 7 CPU test groups passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "VXGIWorld test failed: " << exception.what() << '\n';
        return 1;
    }
}
