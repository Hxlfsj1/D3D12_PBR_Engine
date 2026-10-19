#include "../Resources/VXGIProbeInvalidation.h"
#include <iostream>
#include <random>
#include <stdexcept>

namespace
{
    using Cache = VXGIProbeInvalidation;
    using Float3 = DirectX::XMFLOAT3;
    void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
    VXGIWorld::Cell Cell(int32_t x, int32_t y = 0, int32_t z = 0)
    {
        VXGIWorld::Cell cell;
        cell.coordinate = {x, y, z};
        return cell;
    }
    Cache::InstanceSnapshot Instance(uint32_t id, float x, uint32_t material)
    {
        Cache::InstanceSnapshot value;
        value.id = id;
        value.asset = 100 + id;
        value.boundsMinimum = {x - 0.2f, -0.2f, -0.2f};
        value.boundsMaximum = {x + 0.2f, 0.2f, 0.2f};
        value.trs = {x, 0, 0, 0, 0, 0, 1, 1, 1};
        value.materialIds = {material};
        return value;
    }
    struct Fixture
    {
        Cache cache;
        std::vector<VXGIWorld::Cell> cells = {Cell(-10), Cell(0), Cell(10)};
        std::vector<Cache::InstanceSnapshot> instances = {Instance(11, -5, 0), Instance(12, 5, 1)};
        std::vector<uint64_t> materials = {100, 200};
        uint64_t topology = 1, material = 1, lighting = 1;
        bool Update()
        {
            return cache.Update({cells, instances, 1, topology, material, lighting, {}, materials});
        }
        void Seed()
        {
            Check(Update(), "initial update");
            const auto& domain = cache.GetDomain();
            Check(cache.RecordDependencies(0, Cache::BoxMask(domain, {-5.1f, 0, 0}, {-4.9f, 0, 0}), domain.generation), "left dependency");
            Check(cache.RecordDependencies(1, Cache::BoxMask(domain, {0, 0, 0}, {0, 0, 0}), domain.generation), "middle dependency");
            Check(cache.RecordDependencies(2, Cache::BoxMask(domain, {4.9f, 0, 0}, {5.1f, 0, 0}), domain.generation), "right dependency");
        }
    };
    // Same asset-loader-free geometry shape used by VXGIWorldTests: exercise
    // real SAT allocation and probe padding rather than synthesize halo cells.
    struct TestResource { void* Get() const { return nullptr; } };
    struct TestTexture { uint32_t type = 0; TestResource Resource; std::string path; };
    struct TestVertex { Float3 Position; };
    struct TestMesh
    {
        std::vector<TestVertex> vertices = {{{0.1f, 0.1f, 0.2f}}, {{0.8f, 0.1f, 0.2f}}, {{0.1f, 0.8f, 0.2f}}};
        std::vector<std::vector<unsigned>> lodIndices = {{0, 1, 2}};
        std::vector<TestTexture> textures;
        uint32_t materialID = 7;
        bool isUnlit = false;
        DirectX::XMFLOAT4 baseColorFactor = {1, 1, 1, 1};
        Float3 emissiveFactor = {};
    };
    struct TestModel { std::vector<TestMesh> meshes = {{}}; };
    struct TestInstance
    {
        uint32_t editorId = 1;
        TestModel* pModel = nullptr;
        Float3 translation = {}, rotation = {}, scale = {1, 1, 1};
        uint32_t customMaterialID = UINT32_MAX;
        bool isTransparent = false, isCutout = false;
    };
    struct WorldFixture
    {
        TestModel model;
        std::vector<TestInstance> instances = {{}, {}};
        VXGIWorld world;
        Cache cache;
        std::vector<Cache::InstanceSnapshot> snapshots;
        WorldFixture()
        {
            instances[0].pModel = instances[1].pModel = &model;
            instances[1].editorId = 2;
            instances[1].translation.x = 12;
        }
        void Update(uint32_t layers)
        {
            Check(world.Update(instances, 1, layers), world.GetLastError().c_str());
            snapshots.clear();
            for (const auto& instance : instances)
            {
                Cache::InstanceSnapshot snapshot;
                snapshot.id = instance.editorId;
                snapshot.asset = reinterpret_cast<uintptr_t>(instance.pModel);
                snapshot.trs = {instance.translation.x, instance.translation.y, instance.translation.z,
                    instance.rotation.x, instance.rotation.y, instance.rotation.z,
                    instance.scale.x, instance.scale.y, instance.scale.z};
                snapshot.materialIds = {model.meshes[0].materialID};
                bool first = true;
                for (const auto& triangle : world.GetTriangles())
                    if (triangle.instanceId == instance.editorId)
                        for (const auto point : triangle.positions)
                        {
                            if (first) { snapshot.boundsMinimum = snapshot.boundsMaximum = point; first = false; }
                            snapshot.boundsMinimum = {(std::min)(snapshot.boundsMinimum.x, point.x),
                                (std::min)(snapshot.boundsMinimum.y, point.y), (std::min)(snapshot.boundsMinimum.z, point.z)};
                            snapshot.boundsMaximum = {(std::max)(snapshot.boundsMaximum.x, point.x),
                                (std::max)(snapshot.boundsMaximum.y, point.y), (std::max)(snapshot.boundsMaximum.z, point.z)};
                        }
                Check(!first, "real-world fixture instance has no geometry");
                snapshots.push_back(std::move(snapshot));
            }
            Check(cache.Update({world.GetProbeCells(), snapshots, world.GetVoxelSize(),
                world.GetAllocationRevision(), world.GetMaterialRevision(), 1, {}, {}}), cache.GetLastError().c_str());
        }
        void SeedDependencies()
        {
            const auto& domain = cache.GetDomain();
            const auto& centers = world.GetOccupiedCenters();
            for (uint32_t i = 0; i < centers.size(); ++i)
                Check(cache.RecordDependencies(i, Cache::BoxMask(domain, centers[i], centers[i]), domain.generation),
                    "real-world probe dependency feedback failed");
        }
        void CheckDomainCoverage() const
        {
            const auto& domain = cache.GetDomain();
            for (const auto center : world.GetOccupiedCenters())
            {
                Check(center.x >= domain.origin.x && center.y >= domain.origin.y && center.z >= domain.origin.z &&
                    center.x <= domain.origin.x + domain.cellSize.x * domain.dimensions[0] &&
                    center.y <= domain.origin.y + domain.cellSize.y * domain.dimensions[1] &&
                    center.z <= domain.origin.z + domain.cellSize.z * domain.dimensions[2],
                    "probe halo center outside dependency domain");
                const auto mask = Cache::BoxMask(domain, center, center);
                Check(std::any_of(mask.begin(), mask.end(), [](uint32_t word) { return word != 0; }),
                    "probe halo origin missing from dependency mask");
            }
        }
        void CheckIdle(uint32_t layers)
        {
            const auto revision = cache.GetRevision();
            const auto worldRevision = world.GetRevision();
            for (uint32_t i = 0; i < 32; ++i)
            {
                Update(layers);
                const auto& change = cache.GetChange();
                Check(!change.changed && change.dirtyClassification.empty() && change.dirtySH.empty() && change.historyRemap.empty(),
                    "static padded world emitted new probe work");
            }
            Check(cache.GetRevision() == revision && world.GetRevision() == worldRevision,
                "static padded world changed revision");
        }
        void CheckRemap(const std::vector<VXGIWorld::Cell>& oldCells) const
        {
            const auto& cells = world.GetProbeCells();
            const auto& remap = cache.GetChange().historyRemap;
            Check(remap.size() == cells.size(), "history remap did not include every halo probe");
            for (size_t i = 0; i < cells.size(); ++i)
            {
                const auto old = std::lower_bound(oldCells.begin(), oldCells.end(), cells[i].coordinate,
                    [](const VXGIWorld::Cell& cell, const VXGIWorld::Coordinate& coordinate) { return cell.coordinate < coordinate; });
                const auto expected = old != oldCells.end() && old->coordinate == cells[i].coordinate ?
                    static_cast<uint32_t>(old - oldCells.begin()) : UINT32_MAX;
                Check(remap[i] == expected, "halo history identity changed with compact probe index");
            }
        }
    };
    void InitialAndIdle()
    {
        Fixture f;
        Check(f.Update(), "initial update failed");
        auto change = f.cache.GetChange();
        Check(change.changed && change.domainChanged && change.topologyChanged, "initial change flags");
        Check(change.dirtyClassification == std::vector<uint32_t>({0, 1, 2}), "initial classification all");
        Check(change.historyRemap == std::vector<uint32_t>({UINT32_MAX, UINT32_MAX, UINT32_MAX}), "initial histories new");
        const auto generation = f.cache.GetDomain().generation;
        for (size_t i = 0; i < 4096; ++i)
        {
            Check(f.Update(), "idle update failed");
            Check(!f.cache.GetChange().changed && f.cache.GetChange().dirtyClassification.empty() &&
                f.cache.GetChange().dirtySH.empty() && f.cache.GetChange().historyRemap.empty(), "idle emitted work");
        }
        Check(f.cache.GetDomain().generation == generation && f.cache.GetRevision() == change.revision, "idle changed revisions");
    }
    void LocalGeometryAndMissSegments()
    {
        Fixture f;
        f.Seed();
        const auto generation = f.cache.GetDomain().generation;
        // Probe 0 may be far from the moving object: the recorded path crosses
        // its old location, so source distance and hit identity are irrelevant.
        f.instances[0] = Instance(11, -4.5f, 0);
        Check(f.Update(), "local movement update failed");
        const auto& change = f.cache.GetChange();
        Check(!change.domainChanged && f.cache.GetDomain().generation == generation, "small move rebuilt domain");
        Check(change.dirtyClassification == std::vector<uint32_t>({0}), "small move failed local dependency selection");
        Check(change.dirtySH.empty(), "classification should dominate SH");
        Check(!f.cache.HasDependency(0) && f.cache.HasDependency(1) && f.cache.HasDependency(2), "dirty dependency clearing");
        Check(change.historyRemap == std::vector<uint32_t>({0, 1, 2}), "unchanged topology history remap");

        const auto missed = Cache::BoxMask(f.cache.GetDomain(), {-1, -0.1f, -0.1f}, {4, 0.1f, 0.1f});
        Check(f.cache.RecordDependencies(1, missed, generation), "miss segment seed");
        f.instances[0] = Instance(11, 2, 0);
        Check(f.Update(), "new blocker movement");
        Check(std::find(f.cache.GetChange().dirtyClassification.begin(), f.cache.GetChange().dirtyClassification.end(), 1u) !=
            f.cache.GetChange().dirtyClassification.end(), "new blocker did not invalidate previously missed path");
    }
    void MaterialsAndLight()
    {
        Fixture f;
        f.Seed();
        ++f.materials[0];
        ++f.material;
        Check(f.Update(), "material update");
        Check(f.cache.GetChange().dirtyClassification.empty() && f.cache.GetChange().dirtySH == std::vector<uint32_t>({0}),
            "material payload invalidation was not SH-local");
        Check(f.cache.HasDependency(0), "SH invalidation discarded classification dependency");
        ++f.lighting;
        Check(f.Update(), "light update");
        Check(f.cache.GetChange().dirtyClassification.empty() && f.cache.GetChange().dirtySH == std::vector<uint32_t>({0, 1, 2}),
            "global light must invalidate only all SH");
        f.instances[1].materialIds = {0};
        Check(f.Update(), "material reassignment");
        Check(f.cache.GetChange().dirtyClassification.empty() && f.cache.GetChange().dirtySH == std::vector<uint32_t>({2}),
            "material reassignment invalidation");
    }
    void RemappingAndRemoval()
    {
        Fixture f;
        f.Seed();
        f.cells = {Cell(10), Cell(-10), Cell(1)};
        ++f.topology;
        Check(f.Update(), "reordered topology");
        Check(f.cache.GetChange().historyRemap == std::vector<uint32_t>({2, 0, UINT32_MAX}), "world coordinates did not preserve stable identities");
        Check(f.cache.GetChange().dirtyClassification == std::vector<uint32_t>({2}), "new probe initialization");
        Check(f.cache.HasDependency(0) && f.cache.HasDependency(1) && !f.cache.HasDependency(2), "dependency remap");
        f.instances.erase(f.instances.begin());
        Check(f.Update(), "instance removal");
        Check(f.cache.GetChange().dirtyClassification == std::vector<uint32_t>({1, 2}), "removed instance old bounds not invalidated or unknown omitted");
    }
    void ExpansionAndStaleFeedback()
    {
        Fixture f;
        f.Seed();
        const auto old = f.cache.GetDomain();
        f.instances[0] = Instance(11, 100, 0);
        Check(f.Update(), "domain expansion");
        Check(f.cache.GetChange().domainChanged && f.cache.GetChange().dirtyClassification == std::vector<uint32_t>({0, 1, 2}),
            "out-of-domain movement must invalidate all");
        Check(!f.cache.RecordDependencies(0, {}, old.generation), "old-domain feedback accepted");
        const auto next = f.cache.GetDomain();
        Check(next.origin.x <= old.origin.x && next.origin.x + next.cellSize.x * float(next.dimensions[0]) >=
            old.origin.x + old.cellSize.x * float(old.dimensions[0]), "domain shrank during expansion");
        Check(!f.cache.RecordDependencies(99, {}, next.generation), "out-of-range feedback accepted");
    }
    void BoundaryCoverageAndAccumulation()
    {
        Cache::Domain domain;
        domain.origin = {-4, -2, -4}; domain.cellSize = {1, 1, 1};
        domain.dimensions = {8, 4, 8}; domain.cellCount = 256; domain.generation = 1;
        const auto boundary = Cache::BoxMask(domain, {0, 0, 0}, {0, 0, 0});
        const auto left = Cache::BoxMask(domain, {-0.01f, 0, 0}, {-0.01f, 0, 0});
        const auto right = Cache::BoxMask(domain, {0.01f, 0, 0}, {0.01f, 0, 0});
        Check(Cache::Intersects(boundary, left) && Cache::Intersects(boundary, right), "closed grid boundary lost adjacent cell");
        Check(!Cache::Intersects(Cache::BoxMask(domain, {20, 0, 0}, {21, 0, 0}), boundary), "outside box polluted grid");
        const auto full = Cache::BoxMask(domain, {-100, -100, -100}, {100, 100, 100});
        for (const auto word : full) Check(word == UINT32_MAX, "full grid mask incomplete");
        Fixture f;
        Check(f.Update(), "dependency accumulation setup");
        const auto generation = f.cache.GetDomain().generation;
        const auto a = Cache::BoxMask(f.cache.GetDomain(), {-5, 0, 0}, {-5, 0, 0});
        const auto b = Cache::BoxMask(f.cache.GetDomain(), {5, 0, 0}, {5, 0, 0});
        Check(f.cache.RecordDependencies(0, a, generation) && f.cache.RecordDependencies(0, b, generation), "accumulate feedback");
        Check(Cache::Intersects(f.cache.GetDependency(0), a) && Cache::Intersects(f.cache.GetDependency(0), b), "history dependency overwritten");
        const std::array<uint32_t, 1> inFlight = {0};
        f.cache.MarkDependenciesUnknown(inFlight);
        Check(!f.cache.HasDependency(0), "inflight paths must be treated as unknown");
        Check(f.cache.RecordDependencies(0, {}, generation) && Cache::Intersects(f.cache.GetDependency(0), a) &&
            Cache::Intersects(f.cache.GetDependency(0), b), "inflight completion lost previous classification paths");

        std::mt19937 random(712);
        std::uniform_real_distribution<float> position(-4, 4), radius(0, 2);
        for (size_t i = 0; i < 10000; ++i)
        {
            const Float3 point = {position(random), position(random) * 0.5f, position(random)};
            const Float3 aLow = {point.x - radius(random), point.y - radius(random), point.z - radius(random)};
            const Float3 aHigh = {point.x + radius(random), point.y + radius(random), point.z + radius(random)};
            const Float3 bLow = {point.x - radius(random), point.y - radius(random), point.z - radius(random)};
            const Float3 bHigh = {point.x + radius(random), point.y + radius(random), point.z + radius(random)};
            Check(Cache::Intersects(Cache::BoxMask(domain, aLow, aHigh), Cache::BoxMask(domain, bLow, bHigh)),
                "intersecting closed world boxes lost shared dependency cells");
        }
    }
    void SameBoundsMetadataAndValidation()
    {
        Fixture f;
        f.Seed();
        f.instances[0].trs[3] = 45;
        Check(f.Update() && f.cache.GetChange().dirtyClassification == std::vector<uint32_t>({0}),
            "rotation with same AABB failed geometry invalidation");
        f.Seed();
        f.instances[0].cutout = true;
        Check(f.Update() && f.cache.GetChange().dirtyClassification == std::vector<uint32_t>({0}),
            "coverage flag did not invalidate classification");
        f.Seed();
        ++f.instances[0].coverageFingerprint;
        Check(f.Update() && f.cache.GetChange().dirtyClassification == std::vector<uint32_t>({0}),
            "coverage payload did not invalidate classification");
        const auto revision = f.cache.GetRevision();
        f.instances[0].boundsMinimum.x = (std::numeric_limits<float>::quiet_NaN)();
        Check(!f.Update() && f.cache.GetRevision() == revision, "invalid bounds committed cache state");
    }
    void CachedNeighborVisibilityDependencies()
    {
        Cache cache;
        std::vector<VXGIWorld::Cell> cells = {Cell(0)};
        auto blocker = Instance(1, 1, 0);
        blocker.boundsMinimum = {0.99f, 0.49f, 0.49f};
        blocker.boundsMaximum = {1.01f, 0.51f, 0.51f};
        std::vector<Cache::InstanceSnapshot> instances = {blocker};
        Check(cache.Update({cells, instances, 1, 1, 1, 1, {}, {}}), "neighbor edge setup");
        const auto domain = cache.GetDomain();
        Check(domain.cellSize.x < 1, "neighbor fixture requires sub-voxel coarse cells");
        const auto middle = Cache::BoxMask(domain, blocker.boundsMinimum, blocker.boundsMaximum);
        const auto origin = Cache::BoxMask(domain, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f});
        Check(!Cache::Intersects(origin, middle), "neighbor edge midpoint overlaps the origin coarse cell");
        // No stochastic path covers this blocker; only the cached +X edge does.
        Check(cache.RecordDependencies(0, {}, domain.generation), "empty stochastic dependency feedback");
        Check(Cache::Intersects(cache.GetDependency(0), middle), "cached neighbor edge omitted from dependency mask");
        instances[0].boundsMinimum.y += 0.1f;instances[0].boundsMaximum.y += 0.1f;instances[0].trs[1] += 0.1f;
        Check(cache.Update({cells, instances, 1, 1, 1, 1, {}, {}}) &&
            cache.GetChange().dirtyClassification == std::vector<uint32_t>({0}), "neighbor edge occluder movement did not invalidate visibility");
    }
    void RealWorldProbeHaloRemapping()
    {
        WorldFixture f;
        f.Update(0);
        Check(f.cache.GetChange().dirtyClassification.size() == f.world.GetProbeCells().size(),
            "initial real-world probes were not classified");
        const auto geometryRevision = f.world.GetGeometryRevision();
        for (uint32_t layers = 1; layers <= 2; ++layers)
        {
            f.SeedDependencies();
            const auto oldCells = f.world.GetProbeCells();
            const auto generation = f.cache.GetDomain().generation;
            f.Update(layers);
            const auto& change = f.cache.GetChange();
            Check(change.topologyChanged && !change.domainChanged && f.cache.GetDomain().generation == generation,
                "in-domain halo growth rebuilt dependency domain");
            Check(f.world.GetGeometryRevision() == geometryRevision, "probe halo growth invalidated geometry");
            Check(f.world.GetProbeCells().size() > oldCells.size(), "halo fixture did not grow");
            f.CheckRemap(oldCells);
            Check(change.dirtyClassification.size() == f.world.GetProbeCells().size() - oldCells.size() && change.dirtySH.empty(),
                "halo growth must classify only newly allocated probes");
            for (uint32_t i = 0; i < change.historyRemap.size(); ++i)
            {
                const bool isNew = change.historyRemap[i] == UINT32_MAX;
                const bool isDirty = std::binary_search(change.dirtyClassification.begin(), change.dirtyClassification.end(), i);
                Check(isDirty == isNew && f.cache.HasDependency(i) != isNew, "halo growth lost an existing probe dependency");
            }
            f.CheckDomainCoverage();
            f.SeedDependencies();
            f.CheckIdle(layers);
        }
        // Moving/restoring only the left model reallocates some halo cells. The
        // distant right model's completed histories and dependencies must survive.
        for (const float translation : {1.0f, 0.0f})
        {
            f.SeedDependencies();
            const auto oldCells = f.world.GetProbeCells();
            f.instances[0].translation.x = translation;
            f.Update(2);
            const auto& change = f.cache.GetChange();
            Check(change.topologyChanged && !change.domainChanged && !change.dirtyClassification.empty() &&
                change.dirtyClassification.size() < f.world.GetProbeCells().size() && change.dirtySH.empty(),
                "local padded model movement invalidated the entire cache");
            f.CheckRemap(oldCells);
            for (uint32_t i = 0; i < f.world.GetProbeCells().size(); ++i)
            {
                const bool isDirty = std::binary_search(change.dirtyClassification.begin(), change.dirtyClassification.end(), i);
                if (change.historyRemap[i] == UINT32_MAX) Check(isDirty, "new moved halo probe missed classification");
                if (f.world.GetProbeCells()[i].coordinate.x >= 10)
                    Check(change.historyRemap[i] != UINT32_MAX && !isDirty && f.cache.HasDependency(i),
                        "moving another model discarded distant halo history");
            }
            f.CheckDomainCoverage();
            f.SeedDependencies();
            f.CheckIdle(2);
        }
    }
}

int main()
{
    try
    {
        InitialAndIdle(); LocalGeometryAndMissSegments(); MaterialsAndLight();
        RemappingAndRemoval(); ExpansionAndStaleFeedback(); BoundaryCoverageAndAccumulation();
        SameBoundsMetadataAndValidation();
        CachedNeighborVisibilityDependencies();
        RealWorldProbeHaloRemapping();
        std::cout << "PASS: 9 probe invalidation groups; 4096 idle updates, 10000 shared-cell coverage cases, local old/new/missed-path and cached-neighbor invalidation, material/light separation, stable coordinate remap, domain generations, real World halo 0/1/2 growth and local movement/restoration.\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
