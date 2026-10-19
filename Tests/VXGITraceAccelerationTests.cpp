#include "../Resources/VXGITraceAcceleration.h"
#include "../Resources/VXGISDF.h"
#include <iostream>
#include <random>
#include <stdexcept>

namespace
{
    struct Float3 { float x = 0, y = 0, z = 0; };
    struct Triangle
    {
        std::array<Float3, 3> positions{};
        uint32_t materialId = 0;
        bool isCutout = false;
    };
    using Packed = VXGITraceAcceleration::Packed;
    void Check(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    float Float(uint32_t bits)
    {
        float value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    std::array<double, 3> Vector(Float3 p) { return {p.x, p.y, p.z}; }
    std::array<double, 3> Subtract(std::array<double, 3> a, std::array<double, 3> b)
    {
        return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    }
    std::array<double, 3> Cross(std::array<double, 3> a, std::array<double, 3> b)
    {
        return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    }
    double Dot(std::array<double, 3> a, std::array<double, 3> b)
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }
    bool TriangleIntersection(Float3 origin, Float3 direction, const Triangle& triangle, double maximum)
    {
        const auto a = Vector(triangle.positions[0]);
        const auto e1 = Subtract(Vector(triangle.positions[1]), a), e2 = Subtract(Vector(triangle.positions[2]), a);
        const auto d = Vector(direction), p = Cross(d, e2);
        const double determinant = Dot(e1, p);
        if (std::abs(determinant) <= 1e-12 * std::sqrt(Dot(Cross(e1, e2), Cross(e1, e2)))) return false;
        const auto offset = Subtract(Vector(origin), a), q = Cross(offset, e1);
        const double u = Dot(offset, p) / determinant, v = Dot(d, q) / determinant;
        if (u < -2e-5 || v < -2e-5 || u + v > 1.00002) return false;
        const double distance = Dot(e2, q) / determinant;
        return distance >= 0 && distance <= maximum;
    }
    bool BoxIntersection(const Packed& packed, uint32_t node, Float3 origin, Float3 direction, double maximum)
    {
        double entry = 0, exit = maximum;
        const auto o = Vector(origin), d = Vector(direction);
        for (size_t axis = 0; axis < 3; ++axis)
        {
            const double minimum = Float(packed.references[node + axis]), maximumAxis = Float(packed.references[node + 3 + axis]);
            if (d[axis] == 0)
            {
                if (o[axis] < minimum || o[axis] > maximumAxis) return false;
            }
            else
            {
                const double a = (minimum - o[axis]) / d[axis], b = (maximumAxis - o[axis]) / d[axis];
                entry = (std::max)(entry, (std::min)(a, b));
                exit = (std::min)(exit, (std::max)(a, b));
            }
        }
        return entry <= exit;
    }
    std::vector<uint32_t> GatherCandidates(const Packed& packed, uint32_t cell, Float3 origin, Float3 direction, double maximum)
    {
        std::vector<uint32_t> ids;
        const uint32_t first = packed.offsets[cell], end = packed.offsets[cell + 1];
        if (first == end) return ids;
        if (packed.references[first] != VXGITraceAcceleration::DenseMarker)
            return {packed.references.begin() + first, packed.references.begin() + end};
        uint32_t node = packed.references[first + 2];
        while (node < end)
        {
            const uint32_t escape = packed.references[node + 6], count = packed.references[node + 7];
            if (!BoxIntersection(packed, node, origin, direction, maximum)) node = escape;
            else if (!count) node += VXGITraceAcceleration::NodeWords;
            else
            {
                ids.insert(ids.end(), packed.references.begin() + node + 8, packed.references.begin() + node + 8 + count);
                node = escape;
            }
        }
        Check(node == end, "Stackless traversal did not end at cell end.");
        return ids;
    }
    uint32_t ValidateNode(const Packed& packed, uint32_t node, uint32_t end, std::vector<uint32_t>& ids)
    {
        Check(node + 8 <= end, "Node header leaves its cell.");
        const uint32_t escape = packed.references[node + 6], count = packed.references[node + 7];
        Check(escape > node + 7 && escape <= end, "Invalid escape offset.");
        for (size_t axis = 0; axis < 3; ++axis)
            Check(std::isfinite(Float(packed.references[node + axis])) &&
                Float(packed.references[node + axis]) < Float(packed.references[node + 3 + axis]), "Invalid conservative node bounds.");
        if (count)
        {
            Check(count <= VXGITraceAcceleration::LeafCapacity && escape == node + 8 + count, "Invalid leaf layout.");
            ids.insert(ids.end(), packed.references.begin() + node + 8, packed.references.begin() + escape);
        }
        else
        {
            const uint32_t right = ValidateNode(packed, node + 8, escape, ids);
            Check(right < escape, "Internal node lacks its second child.");
            Check(ValidateNode(packed, right, escape, ids) == escape, "Internal children do not exhaust subtree.");
        }
        return escape;
    }
    std::vector<Triangle> GridTriangles(float offset = 0)
    {
        std::vector<Triangle> triangles;
        for (uint32_t index = 0; index < 96; ++index)
        {
            const float x = float(index % 12) * 0.08f + offset, y = float(index / 12) * 0.1f + offset;
            const float z = offset + float(index % 7) * 0.02f;
            Triangle triangle;
            triangle.positions = {{{x, y, z}, {x + 0.055f, y, z}, {x, y + 0.07f, z}}};
            triangles.push_back(triangle);
        }
        return triangles;
    }
    void TestSmallAndInvalidInput()
    {
        const auto triangles = GridTriangles();
        auto result = VXGITraceAcceleration::Build(std::vector<uint32_t>{0, 0, 2}, std::vector<uint32_t>{0, 1}, triangles, 1.0f);
        Check(result.offsets.empty() && result.references.empty() && result.stats.acceleratedCells == 0 &&
            result.stats.maximumCellCandidates == 2, "Small cells did not preserve the original CSR path.");
        result = VXGITraceAcceleration::Build(std::vector<uint32_t>{}, std::vector<uint32_t>{}, triangles, 1.0f);
        Check(result.offsets.empty() && result.references.empty(), "Empty CSR changed.");
        uint32_t rejected = 0;
        for (const std::vector<uint32_t> offsets : {std::vector<uint32_t>{1, 2}, std::vector<uint32_t>{0, 2, 1}, std::vector<uint32_t>{0, 3}})
        {
            try { VXGITraceAcceleration::Build(offsets, std::vector<uint32_t>{0, 1}, triangles, 1.0f); }
            catch (const std::invalid_argument&) { ++rejected; }
        }
        try { VXGITraceAcceleration::Build(std::vector<uint32_t>{0, 1}, std::vector<uint32_t>{UINT32_MAX}, triangles, 1.0f); }
        catch (const std::invalid_argument&) { ++rejected; }
        try { VXGITraceAcceleration::Build(std::vector<uint32_t>{0}, std::vector<uint32_t>{}, triangles, 0.0f); }
        catch (const std::invalid_argument&) { ++rejected; }
        Check(rejected == 5, "Malformed pack input was accepted.");
    }
    void TestLayoutAndReferenceCompleteness()
    {
        const auto triangles = GridTriangles();
        std::vector<uint32_t> references;
        for (uint32_t index = 0; index < 96; ++index) references.push_back(index);
        references.insert(references.end(), {0, 1, 2});
        for (uint32_t index = 0; index < 96; ++index) references.push_back(index);
        const std::vector<uint32_t> offsets = {0, 0, 96, 99, 195, 195};
        const auto packed = VXGITraceAcceleration::Build(offsets, references, triangles, 1.0f);
        Check(packed.stats.acceleratedCells == 2 && packed.offsets.size() == offsets.size() &&
            packed.offsets.back() == packed.references.size(), "Packed CSR sizes are wrong.");
        for (uint32_t cell = 0; cell + 1 < offsets.size(); ++cell)
        {
            const uint32_t count = offsets[cell + 1] - offsets[cell], first = packed.offsets[cell], end = packed.offsets[cell + 1];
            if (count <= VXGITraceAcceleration::LinearThreshold)
            {
                Check(end - first == count && std::equal(references.begin() + offsets[cell], references.begin() + offsets[cell + 1],
                    packed.references.begin() + first), "Linear cell IDs changed.");
                continue;
            }
            Check(packed.references[first] == VXGITraceAcceleration::DenseMarker && packed.references[first + 1] == count &&
                packed.references[first + 2] == first + 3 + count, "Dense cell header is wrong.");
            Check(std::equal(references.begin() + offsets[cell], references.begin() + offsets[cell + 1],
                packed.references.begin() + first + 3), "Malformed-coverage fallback order changed.");
            std::vector<uint32_t> treeIds;
            Check(ValidateNode(packed, packed.references[first + 2], end, treeIds) == end, "Root escape differs from physical cell end.");
            std::sort(treeIds.begin(), treeIds.end());
            std::vector<uint32_t> original(references.begin() + offsets[cell], references.begin() + offsets[cell + 1]);
            std::sort(original.begin(), original.end());
            Check(treeIds == original, "BVH omitted or duplicated a cell candidate.");
        }
        const auto repeat = VXGITraceAcceleration::Build(offsets, references, triangles, 1.0f);
        Check(repeat.offsets == packed.offsets && repeat.references == packed.references, "Packing is nondeterministic.");
    }
    void TestConservativeCandidateRejection()
    {
        uint64_t tested = 0, retained = 0, original = 0;
        std::mt19937 rng(751);
        std::uniform_real_distribution<float> coordinate(-0.3f, 1.3f);
        for (const float translation : {0.0f, 10000.0f})
        {
            const auto triangles = GridTriangles(translation);
            std::vector<uint32_t> references;
            for (uint32_t index = 0; index < triangles.size(); ++index) references.push_back(index);
            const auto packed = VXGITraceAcceleration::Build(std::vector<uint32_t>{0, uint32_t(references.size())}, references, triangles, 1.0f);
            for (uint32_t ray = 0; ray < 1200; ++ray)
            {
                Float3 origin = {translation + coordinate(rng), translation + coordinate(rng), translation - 1};
                Float3 direction = {0, 0, 1};
                if (ray >= triangles.size() * 3)
                {
                    direction.x = coordinate(rng) * 0.2f;
                    direction.y = coordinate(rng) * 0.2f;
                    if (ray % 2 == 0) { origin.z = translation + 1; direction.z = -1; }
                }
                if (ray < triangles.size() * 3)
                {
                    const auto& triangle = triangles[ray % triangles.size()];
                    const float u = ray % 3 == 0 ? -1.9e-5f : ray % 3 == 1 ? 0.0f : 0.5f;
                    const float v = ray % 3 == 2 ? 0.500019f : 0.5f;
                    origin.x = triangle.positions[0].x + u * (triangle.positions[1].x - triangle.positions[0].x) +
                        v * (triangle.positions[2].x - triangle.positions[0].x);
                    origin.y = triangle.positions[0].y + u * (triangle.positions[1].y - triangle.positions[0].y) +
                        v * (triangle.positions[2].y - triangle.positions[0].y);
                }
                const auto candidates = GatherCandidates(packed, 0, origin, direction, 4);
                retained += candidates.size(); original += references.size(); ++tested;
                for (uint32_t index = 0; index < triangles.size(); ++index)
                    if (TriangleIntersection(origin, direction, triangles[index], 4))
                        Check(std::find(candidates.begin(), candidates.end(), index) != candidates.end(), "BVH rejected an accepted ray/triangle candidate.");
            }
        }
        Check(retained < original / 3, "Synthetic separated geometry gained insufficient candidate pruning.");
        std::cout << "Conservative candidate check: " << tested << " rays, " << retained << "/" << original << " candidates retained.\n";
    }
    void TestMetadataIndependence()
    {
        auto triangles = GridTriangles();
        std::vector<uint32_t> references;
        for (uint32_t index = 0; index < triangles.size(); ++index) references.push_back(index);
        const std::vector<uint32_t> offsets = {0, uint32_t(references.size())};
        const auto original = VXGITraceAcceleration::Build(offsets, references, triangles, 1.0f);
        for (auto& triangle : triangles) { triangle.materialId = 913; triangle.isCutout = true; }
        const auto metadata = VXGITraceAcceleration::Build(offsets, references, triangles, 1.0f);
        Check(original.offsets == metadata.offsets && original.references == metadata.references, "Material metadata changed geometric acceleration.");
        triangles[0].positions[0].x = std::numeric_limits<float>::quiet_NaN();
        bool rejected = false;
        try { VXGITraceAcceleration::Build(offsets, references, triangles, 1.0f); }
        catch (const std::invalid_argument&) { rejected = true; }
        Check(rejected, "Non-finite acceleration bounds were accepted.");
    }
    void TestSDFIntegrationAndRevisionReuse()
    {
        struct Resource { void* Get() const { return nullptr; } };
        struct Texture { uint32_t type = 0; Resource Resource; std::string path; };
        struct Vertex
        {
            DirectX::XMFLOAT3 Position, Normal = {0, 0, 1};
            DirectX::XMFLOAT2 TexCoords = {};
            DirectX::XMFLOAT3 Tangent = {1, 0, 0}, Bitangent = {0, 1, 0};
        };
        struct Mesh
        {
            std::vector<Vertex> vertices;
            std::vector<std::vector<unsigned>> lodIndices = {{}};
            std::vector<Texture> textures;
            uint32_t materialID = 2;
            bool isUnlit = false;
            DirectX::XMFLOAT4 baseColorFactor = {1, 1, 1, 1};
            DirectX::XMFLOAT3 emissiveFactor = {};
        };
        struct Model { std::vector<Mesh> meshes = {{}}; };
        struct Instance
        {
            uint32_t editorId = 73;
            Model* pModel = nullptr;
            DirectX::XMFLOAT3 translation = {}, rotation = {}, scale = {1, 1, 1};
            uint32_t customMaterialID = UINT32_MAX;
            bool isTransparent = false, isCutout = false, isVisible = true, isDirty = false;
            int currentLodLevel = 0;
        };
        Model model;
        for (const auto& triangle : GridTriangles())
        {
            for (const auto& position : triangle.positions)
            {
                const auto index = static_cast<unsigned>(model.meshes[0].vertices.size());
                model.meshes[0].vertices.push_back({{position.x, position.y, position.z}});
                model.meshes[0].lodIndices[0].push_back(index);
            }
        }
        std::vector<Instance> instances = {{}};
        instances[0].pModel = &model;
        VXGIWorld world;
        VXGISDF sdf;
        Check(world.Update(instances, 0.5f) && sdf.Update(world, 1.0f, 128), "Real World/SDF acceleration build failed.");
        Check(sdf.GetTraceAccelerationStats().acceleratedCells > 0 && sdf.GetGPUTraceReferences().size() > sdf.GetTriangleReferences().size(),
            "Dense SDF did not expose packed GPU references.");
        const auto* packedOffsets = sdf.GetGPUCellOffsets().data();
        const auto* packedReferences = sdf.GetGPUTraceReferences().data();
        const auto originalPacked = sdf.GetGPUTraceReferences();
        const auto geometryRevision = sdf.GetGeometryRevision();
        instances[0].customMaterialID = 8;
        instances[0].isCutout = true;
        Check(world.Update(instances, 0.5f) && sdf.Update(world, 1.0f, 128), "Metadata-only SDF update failed.");
        Check(sdf.GetGeometryRevision() == geometryRevision && sdf.GetGPUCellOffsets().data() == packedOffsets &&
            sdf.GetGPUTraceReferences().data() == packedReferences && sdf.GetGPUTraceReferences() == originalPacked,
            "Metadata-only update rebuilt acceleration buffers.");
        Check(sdf.GetTriangles()[0].materialId == 8 && sdf.GetTriangles()[0].isCutout, "Metadata-only update failed to refresh triangle identity.");
        instances[0].translation.x = 0.1f;
        Check(world.Update(instances, 0.5f) && sdf.Update(world, 1.0f, 128), "Geometry SDF update failed.");
        Check(sdf.GetGeometryRevision() == geometryRevision + 1 && sdf.GetGPUTraceReferences() != originalPacked,
            "Geometry update did not rebuild packed bounds.");
        instances.clear();
        Check(world.Update(instances, 0.5f) && sdf.Update(world, 1.0f, 128) && sdf.GetGPUCellOffsets().empty() &&
            sdf.GetGPUTraceReferences().empty() && sdf.GetTraceAccelerationStats().acceleratedCells == 0,
            "Removing all geometry retained stale GPU acceleration.");
    }
}
int main()
{
    try
    {
        TestSmallAndInvalidInput(); TestLayoutAndReferenceCompleteness();
        TestConservativeCandidateRejection(); TestMetadataIndependence(); TestSDFIntegrationAndRevisionReuse();
        std::cout << "VXGITraceAcceleration: all 5 CPU test groups passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "VXGITraceAcceleration failure: " << exception.what() << '\n';
        return 1;
    }
}
