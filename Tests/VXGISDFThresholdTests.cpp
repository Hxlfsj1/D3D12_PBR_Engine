#include "../Resources/VXGISDFThreshold.h"
#include "../Resources/VXGIMeshSDF.h"
#include <iostream>
#include <random>
#include <stdexcept>

namespace
{
    using Float3 = DirectX::XMFLOAT3;
    struct Resource { void* Get() const { return nullptr; } };
    struct Texture { uint32_t type = 0; Resource Resource; std::string path; };
    struct Vertex
    {
        Float3 Position = {}, Normal = { 0, 1, 0 };
        DirectX::XMFLOAT2 TexCoords = {};
        Float3 Tangent = { 1, 0, 0 }, Bitangent = { 0, 0, 1 };
    };
    struct Mesh
    {
        std::vector<Vertex> vertices;
        std::vector<std::vector<unsigned>> lodIndices = { {} };
        std::vector<Texture> textures;
        uint32_t materialID = 7;
        bool isUnlit = false;
        DirectX::XMFLOAT4 baseColorFactor = { 0, 0, 0, 1 };
        Float3 emissiveFactor = {};
    };
    struct Model { std::vector<Mesh> meshes; };
    using Threshold = VXGISDFThreshold;
    using Status = Threshold::Status;
    uint64_t sampleComparisons = 0, marchComparisons = 0;
    void Check(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    bool Near(float a, float b, float tolerance = 2e-4f)
    {
        return std::abs(a - b) <= tolerance;
    }
    Float3 Advance(Float3 origin, Float3 direction, float distance)
    {
        return { origin.x + direction.x * distance, origin.y + direction.y * distance,
            origin.z + direction.z * distance };
    }
    Float3 Normalize(Float3 vector)
    {
        const float length = std::sqrt(vector.x * vector.x + vector.y * vector.y + vector.z * vector.z);
        return { vector.x / length, vector.y / length, vector.z / length };
    }
    void Quad(Mesh& mesh, Float3 a, Float3 b, Float3 c, Float3 d)
    {
        const auto first = static_cast<unsigned>(mesh.vertices.size());
        mesh.vertices.insert(mesh.vertices.end(), { { a }, { b }, { c }, { d } });
        mesh.lodIndices[0].insert(mesh.lodIndices[0].end(),
            { first, first + 1, first + 2, first, first + 2, first + 3 });
    }
    Model Plane(float z = 0.25f)
    {
        Mesh mesh;
        Quad(mesh, { -2, -2, z }, { 2, -2, z }, { 2, 2, z }, { -2, 2, z });
        return { { std::move(mesh) } };
    }
    Model Box(float halfExtent = 1)
    {
        Mesh mesh;
        const float h = halfExtent;
        Quad(mesh, { -h, -h, h }, { h, -h, h }, { h, h, h }, { -h, h, h });
        Quad(mesh, { -h, -h, -h }, { -h, h, -h }, { h, h, -h }, { h, -h, -h });
        Quad(mesh, { h, -h, -h }, { h, h, -h }, { h, h, h }, { h, -h, h });
        Quad(mesh, { -h, -h, -h }, { -h, -h, h }, { -h, h, h }, { -h, h, -h });
        Quad(mesh, { -h, h, -h }, { -h, h, h }, { h, h, h }, { h, h, -h });
        Quad(mesh, { -h, -h, -h }, { h, -h, -h }, { h, -h, h }, { -h, -h, h });
        return { { std::move(mesh) } };
    }
    struct AuthoredField
    {
        Threshold::Grid grid = {};
        std::vector<float> nodes;
        template<class Distance>
        AuthoredField(Float3 origin, float cellSize, std::array<uint32_t, 3> cells, Distance distance)
        {
            grid.origin = origin;
            grid.cellSize = cellSize;
            grid.cellDimensions = cells;
            grid.boundsMaximum = { origin.x + float(cells[0]) * cellSize,
                origin.y + float(cells[1]) * cellSize, origin.z + float(cells[2]) * cellSize };
            for (uint32_t z = 0; z <= cells[2]; ++z)
                for (uint32_t y = 0; y <= cells[1]; ++y)
                    for (uint32_t x = 0; x <= cells[0]; ++x)
                        nodes.push_back(distance(Float3{ origin.x + float(x) * cellSize,
                            origin.y + float(y) * cellSize, origin.z + float(z) * cellSize }));
        }
    };

    // An independent, double interpolation oracle checks the float sampler,
    // rather than relying on the production conservative skipping function.
    double ReferenceSample(const AuthoredField& field, Float3 position)
    {
        const double p[3] = { position.x, position.y, position.z };
        const double origin[3] = { field.grid.origin.x, field.grid.origin.y, field.grid.origin.z };
        std::array<uint32_t, 3> cell = {};
        double fraction[3] = {};
        for (size_t axis = 0; axis < 3; ++axis)
        {
            const double coordinate = (p[axis] - origin[axis]) / field.grid.cellSize;
            cell[axis] = (std::min)(uint32_t(std::floor(coordinate)), field.grid.cellDimensions[axis] - 1);
            fraction[axis] = coordinate - cell[axis];
        }
        double value = 0;
        for (uint32_t z = 0; z < 2; ++z)
            for (uint32_t y = 0; y < 2; ++y)
                for (uint32_t x = 0; x < 2; ++x)
                {
                    const double weight = (x ? fraction[0] : 1 - fraction[0]) *
                        (y ? fraction[1] : 1 - fraction[1]) * (z ? fraction[2] : 1 - fraction[2]);
                    const uint32_t index = cell[0] + x + (field.grid.cellDimensions[0] + 1) *
                        (cell[1] + y + (field.grid.cellDimensions[1] + 1) * (cell[2] + z));
                    value += weight * field.nodes[index];
                }
        return value;
    }

    Threshold::Grid FieldGrid(const VXGISDF& sdf)
    {
        const auto& source = sdf.GetGrid();
        Threshold::Grid grid = {};
        grid.origin = source.origin;
        grid.cellSize = source.cellSize;
        grid.cellDimensions = source.cellDimensions;
        grid.boundsMaximum = source.boundsMaximum;
        return grid;
    }
    void CheckFiniteHit(const Threshold::Result& hit, Float3 origin, Float3 direction,
        float minimum, float maximum, float threshold)
    {
        Check(hit.status == Status::Hit, "expected a distance-threshold hit");
        Check(hit.distance >= minimum && hit.distance <= maximum && std::isfinite(hit.distance),
            "threshold hit escaped the original finite interval");
        Check(hit.fieldDistance <= threshold && hit.fieldDistance >= 0,
            "threshold hit was not accepted from the sampled distance");
        const auto expected = Advance(origin, Normalize(direction), hit.distance);
        Check(Near(hit.position.x, expected.x) && Near(hit.position.y, expected.y) && Near(hit.position.z, expected.z),
            "threshold hit changed the original ray");
        ++marchComparisons;
    }
    void TestInterpolationAndDomain()
    {
        AuthoredField field({ -1, -2, -3 }, 0.5f, { 4, 6, 8 }, [](Float3 p)
            { return 1.0f + 0.25f * (p.x + 1) + 0.125f * (p.y + 2) + 0.0625f * (p.z + 3); });
        std::mt19937 random(0x53444654u);
        std::uniform_real_distribution<float> unit(0, 1);
        for (uint32_t i = 0; i < 512; ++i)
        {
            const Float3 p = { -1 + 2 * unit(random), -2 + 3 * unit(random), -3 + 4 * unit(random) };
            const auto sample = Threshold::SampleDistance(field.grid, field.nodes, p);
            Check(sample.valid && std::abs(double(sample.distance) - ReferenceSample(field, p)) <= 5e-6,
                "FP32 distance sampling differs from independent double trilinear interpolation");
            Check(sample.safeDistance >= 0 && sample.safeDistance <= sample.distance &&
                sample.uncertainty >= 0 && sample.uncertainty <= 0.866026f * field.grid.cellSize,
                "sample did not preserve conservative stepping bounds");
            ++sampleComparisons;
        }
        for (const Float3 p : { field.grid.origin, field.grid.boundsMaximum })
            Check(Threshold::SampleDistance(field.grid, field.nodes, p).valid,
                "closed field domain excluded a boundary node");
        Check(!Threshold::SampleDistance(field.grid, field.nodes, { -1.001f, 0, 0 }).valid &&
            !Threshold::SampleDistance(field.grid, field.nodes, { 0, 0, 1.001f }).valid,
            "out-of-domain sample was clamped into a false hit");
        auto withOffset = field.nodes;
        withOffset.insert(withOffset.begin(), 7, -1000);
        auto grid = field.grid;
        grid.nodeOffset = 7;
        const Float3 p = { 0.125f, -0.75f, -1.375f };
        Check(Near(Threshold::SampleDistance(grid, withOffset, p).distance,
            Threshold::SampleDistance(field.grid, field.nodes, p).distance), "shared-asset node offset was ignored");
        withOffset.pop_back();
        Check(!Threshold::SampleDistance(grid, withOffset, p).valid,
            "incomplete field storage was accepted");
    }
    void TestThinSubcellAndSkipBound()
    {
        AuthoredField plane({ 0, 0, 0 }, 1, { 1, 1, 1 }, [](Float3 p) { return std::abs(p.z - 0.4f); });
        const auto surface = Threshold::SampleDistance(plane.grid, plane.nodes, { 0.5f, 0.5f, 0.4f });
        Check(surface.valid && Near(surface.distance, 0.48f), "off-lattice unsigned plane interpolation changed");
        const auto planeHit = Threshold::TraceLocal(plane.grid, plane.nodes, { 0.5f, 0.5f, 0.4f }, { 0, 0, 1 }, 0, 0);
        CheckFiniteHit(planeHit, { 0.5f, 0.5f, 0.4f }, { 0, 0, 1 }, 0, 0, 0.9f);

        AuthoredField point({ 0, 0, 0 }, 1, { 1, 1, 1 }, [](Float3 p)
            { return std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z); });
        const Float3 middle = { 0.5f, 0.5f, 0.5f };
        const auto pointSample = Threshold::SampleDistance(point.grid, point.nodes, middle);
        Check(pointSample.valid && pointSample.distance > 1.12f,
            "point UDF fixture does not distinguish interpolation from skip lower bound");
        // Its nearest-node skip bound is zero (the corner contains a surface),
        // while the actual trilinear sample is above the accepted threshold.
        const auto skipZero = Threshold::TraceLocal(point.grid, point.nodes, middle, { 0, 0, 1 }, 0, 0);
        Check(skipZero.status == Status::Miss && skipZero.sampleCount == 1,
            "zero skip lower bound was falsely treated as a surface hit");

        AuthoredField hiddenPoint({ 0, 0, 0 }, 1, { 1, 1, 1 }, [](Float3 p)
            { const auto q = Advance(p, { -1, -1, -1 }, 0.5f); return std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z); });
        const auto worstCase = Threshold::SampleDistance(hiddenPoint.grid, hiddenPoint.nodes, middle);
        Check(worstCase.valid && Near(worstCase.distance, std::sqrt(0.75f)),
            "subcell point did not exercise worst-case interpolation uncertainty");
        CheckFiniteHit(Threshold::TraceLocal(hiddenPoint.grid, hiddenPoint.nodes, middle, { 1, 0, 0 }, 0, 0),
            middle, { 1, 0, 0 }, 0, 0, 0.9f);
    }
    void TestFiniteIntervalsAndThresholdBoundary()
    {
        AuthoredField plane({ -2, -2, -4 }, 0.25f, { 16, 16, 32 }, [](Float3 p) { return std::abs(p.z - 0.125f); });
        const Float3 origin = { 0, 0, -3.5f }, direction = { 0, 0, 7 };
        const auto result = Threshold::TraceLocal(plane.grid, plane.nodes, origin, direction, 0, 4);
        CheckFiniteHit(result, origin, direction, 0, 4, 0.225f);
        Check(std::abs(result.position.z - 0.125f) <= 0.2251f, "plane hit exceeded its local threshold shell");
        Check(Threshold::TraceLocal(plane.grid, plane.nodes, origin, direction, 0, 2).status == Status::Miss,
            "finite short ray was extended toward a surface");
        CheckFiniteHit(Threshold::TraceLocal(plane.grid, plane.nodes, { 0, 0, -1 }, direction, 1, 1),
            { 0, 0, -1 }, direction, 1, 1, 0.225f);
        Check(Threshold::TraceLocal(plane.grid, plane.nodes, { 0, 0, -1 }, direction, 0.875f, 0.875f).status == Status::Miss,
            "zero-width sample outside threshold shell was accepted");
        Check(Threshold::TraceLocal(plane.grid, plane.nodes, { 5, 0, 0 }, { 1, 0, 0 }, 0, 1).status == Status::Miss,
            "ray outside field domain fabricated a clamped hit");

        AuthoredField equal({ 0, 0, 0 }, 1, { 1, 1, 1 }, [](Float3) { return 0.9f; });
        CheckFiniteHit(Threshold::TraceLocal(equal.grid, equal.nodes, { 0, 0, 0 }, { 1, 0, 0 }, 0, 0),
            { 0, 0, 0 }, { 1, 0, 0 }, 0, 0, 0.9f);
        std::fill(equal.nodes.begin(), equal.nodes.end(), std::nextafter(0.9f, 1.0f));
        Check(Threshold::TraceLocal(equal.grid, equal.nodes, { 0, 0, 0 }, { 1, 0, 0 }, 0, 0).status == Status::Miss,
            "sample above threshold was accepted");
    }
    void TestBoundedTerminationAndInvalidInputs()
    {
        AuthoredField plane({ -1, -1, -8 }, 0.25f, { 8, 8, 64 }, [](Float3 p) { return std::abs(p.z); });
        Threshold::Parameters parameters;
        parameters.maxSteps = 1;
        const auto exhausted = Threshold::TraceLocal(plane.grid, plane.nodes, { 0, 0, -7 }, { 0, 0, 1 }, 0, 14, parameters);
        Check(exhausted.status == Status::Exhausted && exhausted.sampleCount == 1,
            "step budget exhaustion was misreported as a completed miss");
        AuthoredField constant({ 0, 0, 0 }, 1, { 32, 1, 1 }, [](Float3) { return 2.0f; });
        const auto stalled = Threshold::TraceLocal(constant.grid, constant.nodes,
            { -100000000.0f, 0.5f, 0.5f }, { 1, 0, 0 }, 100000000.0f, 100000032.0f);
        Check(stalled.status == Status::Stalled && stalled.sampleCount > 0 && stalled.sampleCount <= 512,
            "FP32 parameter stagnation was hidden or treated as a hit");
        const auto invalid = [&](Float3 origin, Float3 direction, float minimum, float maximum, Threshold::Parameters p)
        {
            Check(Threshold::TraceLocal(plane.grid, plane.nodes, origin, direction, minimum, maximum, p).status == Status::Invalid,
                "invalid trace inputs were accepted");
        };
        invalid({}, {}, 0, 1, {});
        invalid({}, { 0, 0, 1 }, 2, 1, {});
        invalid({}, { 0, 0, 1 }, -1, 1, {});
        invalid({ std::numeric_limits<float>::quiet_NaN(), 0, 0 }, { 0, 0, 1 }, 0, 1, {});
        parameters = {};
        parameters.hitThresholdCells = 0.8f;
        invalid({}, { 0, 0, 1 }, 0, 1, parameters);
        parameters = {};
        parameters.stepScale = 1.01f;
        invalid({}, { 0, 0, 1 }, 0, 1, parameters);
        parameters = {};
        parameters.maxSteps = 0;
        invalid({}, { 0, 0, 1 }, 0, 1, parameters);
        auto brokenGrid = plane.grid;
        brokenGrid.cellDimensions[0] = 0;
        Check(!Threshold::SampleDistance(brokenGrid, plane.nodes, {}).valid, "empty grid was accepted");
        brokenGrid = plane.grid;
        brokenGrid.nodeOffset = UINT32_MAX;
        Check(!Threshold::SampleDistance(brokenGrid, plane.nodes, {}).valid, "overflowing node storage range was accepted");
    }
    void TestBakedOpenAndClosedModels()
    {
        auto plane = Plane(0.1375f);
        auto box = Box();
        VXGIMeshSDFCache cache;
        const auto planeAsset = cache.GetOrCreate(plane, { 0.5f, 128 });
        const auto boxAsset = cache.GetOrCreate(box, { 0.25f, 128 });
        Check(planeAsset && boxAsset, cache.GetLastError().c_str());
        const auto planeGrid = FieldGrid(planeAsset->GetSDF());
        const auto planeSample = Threshold::SampleDistance(planeGrid, planeAsset->GetSDF().GetNodes(), { 0, 0, 0.1375f });
        Check(planeSample.valid && planeSample.distance > 0.1f && planeSample.distance < 0.45f,
            "real baked thin plane fixture lost unsigned interpolation floor");
        const Float3 origin = { 0, 0, -2 }, direction = { 0, 0, 1 };
        CheckFiniteHit(Threshold::TraceLocal(planeGrid, planeAsset->GetSDF().GetNodes(), origin, direction, 0, 4),
            origin, direction, 0, 4, 0.45f);
        const auto grid = FieldGrid(boxAsset->GetSDF());
        const std::array<std::pair<Float3, Float3>, 6> faceRays = {{
            {{ -3, 0, 0 }, { 1, 0, 0 }}, {{ 3, 0, 0 }, { -1, 0, 0 }},
            {{ 0, -3, 0 }, { 0, 1, 0 }}, {{ 0, 3, 0 }, { 0, -1, 0 }},
            {{ 0, 0, -3 }, { 0, 0, 1 }}, {{ 0, 0, 3 }, { 0, 0, -1 }} }};
        for (const auto& [o, d] : faceRays)
        {
            const auto hit = Threshold::TraceLocal(grid, boxAsset->GetSDF().GetNodes(), o, d, 0, 6);
            CheckFiniteHit(hit, o, d, 0, 6, 0.225f);
            const float faceCoordinate = (std::max)({ std::abs(hit.position.x), std::abs(hit.position.y), std::abs(hit.position.z) });
            Check(std::abs(faceCoordinate - 1) <= 0.2251f,
                "baked closed-box hit exceeded its accepted threshold shell");
        }
        Check(cache.GetStats().builds == 2, "threshold queries caused asset rebakes");
    }
    void TestRandomFinitePlaneRays()
    {
        AuthoredField plane({ -2, -2, -2 }, 0.25f, { 16, 16, 16 }, [](Float3 p) { return std::abs(p.z - 0.375f); });
        std::mt19937 random(0x504c414eu);
        std::uniform_real_distribution<float> coordinate(-0.75f, 0.75f), slope(-0.2f, 0.2f);
        for (uint32_t i = 0; i < 512; ++i)
        {
            const Float3 origin = { coordinate(random), coordinate(random), -1.5f };
            const Float3 direction = { slope(random), slope(random), 1 };
            const auto unit = Normalize(direction);
            const auto hit = Threshold::TraceLocal(plane.grid, plane.nodes, origin, direction, 0, 3);
            CheckFiniteHit(hit, origin, direction, 0, 3, 0.225f);
            const float shellEntry = (0.375f - 0.225f - origin.z) / unit.z;
            Check(hit.distance >= shellEntry - 1e-4f && std::abs(hit.position.z - 0.375f) <= 0.2251f,
                "random plane ray accepted outside its analytic threshold shell");
            Check(hit.sampleCount > 0 && hit.sampleCount <= 512, "random plane ray escaped bounded step count");
        }
    }
}

int main()
{
    try
    {
        TestInterpolationAndDomain();
        TestThinSubcellAndSkipBound();
        TestFiniteIntervalsAndThresholdBoundary();
        TestBoundedTerminationAndInvalidInputs();
        TestBakedOpenAndClosedModels();
        TestRandomFinitePlaneRays();
        std::cout << "VXGISDFThreshold: all 6 CPU test groups passed; " << sampleComparisons <<
            " independent interpolation comparisons, " << marchComparisons << " finite hits.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "VXGISDFThreshold: " << error.what() << '\n';
        return 1;
    }
}
