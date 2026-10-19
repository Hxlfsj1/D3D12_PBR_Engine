#include "../Resources/VXGIMeshSDFThreshold.h"
#include <iostream>
#include <random>
#include <stdexcept>

namespace
{
    struct Resource { void* Get() const { return nullptr; } };
    struct Texture { uint32_t type = 0; Resource Resource; std::string path; };
    struct Vertex
    {
        DirectX::XMFLOAT3 Position = {}, Normal = { 0, 1, 0 };
        DirectX::XMFLOAT2 TexCoords = {};
        DirectX::XMFLOAT3 Tangent = { 1, 0, 0 }, Bitangent = { 0, 0, 1 };
    };
    struct Mesh
    {
        std::vector<Vertex> vertices;
        std::vector<std::vector<unsigned>> lodIndices = { {} };
        std::vector<Texture> textures;
        uint32_t materialID = 7;
        bool isUnlit = false;
        DirectX::XMFLOAT4 baseColorFactor = { 0, 0, 0, 1 };
        DirectX::XMFLOAT3 emissiveFactor = {};
    };
    struct Model { std::vector<Mesh> meshes; };
    struct Instance
    {
        uint32_t editorId = 0;
        Model* pModel = nullptr;
        DirectX::XMFLOAT3 translation = {}, rotation = {}, scale = { 1, 1, 1 };
        uint32_t customMaterialID = UINT32_MAX;
        bool isTransparent = false, isCutout = false, isVisible = true, isDirty = false;
        int currentLodLevel = 0;
    };
    using Scene = VXGIMeshSDFScene;
    using Settings = VXGIMeshSDFCache::BuildSettings;
    constexpr Settings BuildSettings = { 0.5f, 128 };

    void Check(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    bool Near(double a, double b, double tolerance = 2e-4)
    {
        return std::abs(a - b) <= tolerance;
    }
    bool Near(DirectX::XMFLOAT3 a, DirectX::XMFLOAT3 b, double tolerance = 2e-4)
    {
        return Near(a.x, b.x, tolerance) && Near(a.y, b.y, tolerance) && Near(a.z, b.z, tolerance);
    }
    DirectX::XMFLOAT3 Normalize(DirectX::XMFLOAT3 vector)
    {
        const double length = std::sqrt(double(vector.x) * vector.x + double(vector.y) * vector.y + double(vector.z) * vector.z);
        return { float(vector.x / length), float(vector.y / length), float(vector.z / length) };
    }
    DirectX::XMFLOAT3 Advance(DirectX::XMFLOAT3 origin, DirectX::XMFLOAT3 direction, float distance)
    {
        return { origin.x + direction.x * distance, origin.y + direction.y * distance, origin.z + direction.z * distance };
    }
    void Quad(Mesh& mesh, DirectX::XMFLOAT3 a, DirectX::XMFLOAT3 b,
        DirectX::XMFLOAT3 c, DirectX::XMFLOAT3 d)
    {
        const auto first = static_cast<unsigned>(mesh.vertices.size());
        mesh.vertices.insert(mesh.vertices.end(), { { a }, { b }, { c }, { d } });
        mesh.lodIndices[0].insert(mesh.lodIndices[0].end(),
            { first, first + 1, first + 2, first, first + 2, first + 3 });
    }
    Model Box()
    {
        Mesh first, second;
        first.materialID = 17;
        second.materialID = 31;
        Quad(first, { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 });
        Quad(first, { -1, -1, -1 }, { -1, 1, -1 }, { 1, 1, -1 }, { 1, -1, -1 });
        Quad(first, { 1, -1, -1 }, { 1, 1, -1 }, { 1, 1, 1 }, { 1, -1, 1 });
        Quad(second, { -1, -1, -1 }, { -1, -1, 1 }, { -1, 1, 1 }, { -1, 1, -1 });
        Quad(second, { -1, 1, -1 }, { -1, 1, 1 }, { 1, 1, 1 }, { 1, 1, -1 });
        Quad(second, { -1, -1, -1 }, { 1, -1, -1 }, { 1, -1, 1 }, { -1, -1, 1 });
        return { { std::move(first), std::move(second) } };
    }
    Model Plane(float z)
    {
        Mesh mesh;
        Quad(mesh, { -1, -1, z }, { 1, -1, z }, { 1, 1, z }, { -1, 1, z });
        return { { std::move(mesh) } };
    }
    Instance MakeInstance(Model& model, uint32_t id, DirectX::XMFLOAT3 translation = {},
        DirectX::XMFLOAT3 scale = { 1, 1, 1 }, DirectX::XMFLOAT3 rotation = {})
    {
        Instance instance;
        instance.pModel = &model;
        instance.editorId = id;
        instance.translation = translation;
        instance.scale = scale;
        instance.rotation = rotation;
        return instance;
    }
    void BuildReference(const std::vector<Instance>& instances, VXGIWorld& world, VXGISDF& reference)
    {
        Check(world.Update(instances, 1), world.GetLastError().c_str());
        Check(reference.Update(world, 1, 128), reference.GetLastError().c_str());
    }
    void Compare(const Scene& scene, const VXGIWorld& world, const VXGISDF& reference,
        DirectX::XMFLOAT3 origin, DirectX::XMFLOAT3 direction, float minimum, float maximum)
    {
        const auto normalized = Normalize(direction);
        const auto expected = reference.TraceReference(Advance(origin, normalized, minimum), normalized, maximum - minimum);
        const auto actual = scene.TraceGeometry(origin, direction, minimum, maximum);
        Check(actual.hit == expected.hit, "mesh-local/world-BVH hit mismatch");
        if (!expected.hit) return;
        const auto& source = world.GetTriangles()[expected.triangleIndex];
        Check(Near(actual.distance, expected.distance + minimum) && Near(actual.position, expected.position),
            "mesh-local/world-BVH distance or position mismatch");
        Check(actual.instanceId == expected.instanceId && actual.sourceInstanceIndex == source.sourceInstanceIndex &&
            actual.materialId == expected.materialId && actual.meshIndex == source.meshIndex &&
            actual.sourceTriangleIndex == source.sourceTriangleIndex && actual.isCutout == source.isCutout,
            "mesh-local hit lost instance or source surface identity");
        Check(Near(actual.geometricNormal, expected.geometricNormal, 2e-5) && actual.backFace == expected.backFace,
            "mesh-local/world-BVH geometric normal or back-face mismatch");
        Check(Near(actual.barycentrics, expected.barycentrics, 3e-4),
            "mesh-local/world-BVH barycentric mismatch");
        Check(actual.instanceIndex < scene.GetInstances().size() &&
            scene.GetInstances()[actual.instanceIndex].instanceId == actual.instanceId &&
            actual.triangleIndex < scene.GetInstances()[actual.instanceIndex].asset->GetSDF().GetTriangles().size(),
            "hit contains an invalid compact instance or local triangle index");
    }
    void TestWorldBoundsAndFiltering()
    {
        auto model = Box();
        VXGIMeshSDFCache cache;
        Scene scene;
        std::vector<Instance> instances = { MakeInstance(model, 30, { 2, -3, 5 }, { -2, 0.5f, 1.5f }, { 0.4f, 0.2f, -0.7f }),
            MakeInstance(model, 10, { -5, 0, 0 }) };
        Check(scene.UpdateInstances(instances, cache, BuildSettings), scene.GetLastError().c_str());
        Check(scene.GetInstances().size() == 2 && scene.GetInstances()[0].instanceId == 10 &&
            scene.GetInstances()[0].sourceInstanceIndex == 1 && scene.GetInstances()[1].instanceId == 30 &&
            scene.GetInstances()[1].sourceInstanceIndex == 0, "instance records are not stably sorted with source indices");
        VXGIWorld world;
        VXGISDF reference;
        BuildReference(instances, world, reference);
        for (const auto& triangle : world.GetTriangles())
        {
            const auto& record = scene.GetInstances()[triangle.instanceId == 10 ? 0 : 1];
            for (const auto p : triangle.positions)
                Check(p.x >= record.boundsMinimum.x - 1e-5 && p.x <= record.boundsMaximum.x + 1e-5 &&
                    p.y >= record.boundsMinimum.y - 1e-5 && p.y <= record.boundsMaximum.y + 1e-5 &&
                    p.z >= record.boundsMinimum.z - 1e-5 && p.z <= record.boundsMaximum.z + 1e-5,
                    "world instance AABB does not enclose transformed model geometry");
        }
        for (const auto& record : scene.GetInstances())
        {
            const auto& bounds = record.asset->GetTraceBounds();
            for (uint32_t corner = 0; corner < 8; ++corner)
            {
                DirectX::XMFLOAT3 p = {corner & 1 ? bounds.maximum.x : bounds.minimum.x,
                    corner & 2 ? bounds.maximum.y : bounds.minimum.y, corner & 4 ? bounds.maximum.z : bounds.minimum.z};
                DirectX::XMStoreFloat3(&p, DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&p),
                    DirectX::XMLoadFloat4x4(&record.worldFromLocal)));
                Check(p.x >= record.boundsMinimum.x && p.x <= record.boundsMaximum.x &&
                    p.y >= record.boundsMinimum.y && p.y <= record.boundsMaximum.y &&
                    p.z >= record.boundsMinimum.z && p.z <= record.boundsMaximum.z,
                    "world bounds truncated a transformed conservative geometry-bound corner");
            }
        }
        Scene::TraceStats rejected;
        Check(!scene.TraceGeometry({ 100, 100, 100 }, { 0, 0, 1 }, 0, 10, &rejected).hit &&
            rejected.boxTests == 2 && rejected.boxHits == 0 && rejected.localTraces == 0,
            "world AABB miss unnecessarily entered a local SDF trace");
        Scene::TraceStats accepted;
        const auto hit = scene.TraceGeometry({ -5, 0.13f, -4 }, { 0, 0, 1 }, 0, 10, &accepted);
        Check(hit.hit && hit.instanceId == 10 && accepted.boxHits >= 1 && accepted.localTraces >= 1,
            "world AABB filtering discarded a valid local trace");
    }
    void TestFloorRejectsOutgoingRays()
    {
        Mesh mesh;
        Quad(mesh, {-5, 0, -5}, {5, 0, -5}, {5, 0, 5}, {-5, 0, 5});
        Model floor = {{std::move(mesh)}};
        VXGIMeshSDFCache cache;
        Scene scene;
        const std::vector<Instance> instances = {MakeInstance(floor, 1, {}, {2, 2, 2})};
        Check(scene.UpdateInstances(instances, cache, {1, 128}), scene.GetLastError().c_str());
        Scene::TraceStats outward;
        Check(!scene.TraceGeometry({0, 0, 0}, {0, 1, 0}, .0001f, .5f, &outward).hit &&
            outward.boxTests == 1 && outward.boxHits == 0 && outward.localTraces == 0 && outward.triangleTests == 0,
            "a floor-outgoing ray entered the empty padded SDF volume");
        Scene::TraceStats inward;
        const auto hit = scene.TraceGeometry({.1f, .25f, .2f}, {0, -1, 0}, .0001f, .5f, &inward);
        Check(hit.hit && Near(hit.distance, .25) && inward.boxHits == 1 && inward.triangleTests > 0,
            "tight floor bounds rejected a valid downward surface hit");
        const auto& bounds = scene.GetInstances().front().asset->GetTraceBounds();
        Check(bounds.minimum.y > -.00002f && bounds.maximum.y < .00002f,
            "planar geometry retained cell-sized padding along its normal");
    }
    void TestFiniteWorldIntervalsAndStarts()
    {
        auto model = Box();
        VXGIMeshSDFCache cache;
        Scene scene;
        std::vector<Instance> instances = { MakeInstance(model, 41) };
        Check(scene.UpdateInstances(instances, cache, BuildSettings), scene.GetLastError().c_str());
        const DirectX::XMFLOAT3 origin = { 0.13f, 0.17f, -3 }, direction = { 0, 0, 8 };
        Check(!scene.TraceGeometry(origin, direction, 0, 1.999f).hit, "world finite ray was extended beyond rayMax");
        auto hit = scene.TraceGeometry(origin, direction, 0, 2);
        Check(hit.hit && Near(hit.distance, 2) && !hit.backFace, "inclusive rayMax or direction normalization failed");
        hit = scene.TraceGeometry(origin, direction, 2, 2);
        Check(hit.hit && Near(hit.distance, 2), "zero-length interval at the surface missed");
        Check(!scene.TraceGeometry(origin, direction, 2.001f, 3.999f).hit,
            "a surface before rayMin was accepted or rayMax was extended");
        hit = scene.TraceGeometry(origin, direction, 2.001f, 4);
        Check(hit.hit && Near(hit.distance, 4) && hit.backFace,
            "rayMin did not preserve the farther valid surface/world distance");
        hit = scene.TraceGeometry({ 0.13f, 0.17f, 0 }, { 0, 0, 1 }, 0, 2);
        Check(hit.hit && Near(hit.distance, 1) && hit.backFace, "origin inside a model failed");
        hit = scene.TraceGeometry({ 0.13f, 0.17f, -1 }, { 0, 0, -1 }, 0, 0);
        Check(hit.hit && Near(hit.distance, 0), "surface-origin zero range failed");
        Check(!scene.TraceGeometry(origin, direction, -1, 4).hit &&
            !scene.TraceGeometry(origin, direction, 3, 2).hit &&
            !scene.TraceGeometry(origin, {}, 0, 4).hit &&
            !scene.TraceGeometry(origin, direction, 0, std::numeric_limits<float>::infinity()).hit &&
            !scene.TraceGeometry(origin, direction, std::numeric_limits<float>::quiet_NaN(), 4).hit &&
            !scene.TraceGeometry({ std::numeric_limits<float>::quiet_NaN(), 0, 0 }, direction, 0, 4).hit,
            "invalid ray or world interval was accepted");
        // The same world ray segment must remain finite under nonuniform scale.
        instances[0].scale = { 0.3f, 3, 4 };
        Check(scene.UpdateInstances(instances, cache, BuildSettings), scene.GetLastError().c_str());
        Check(!scene.TraceGeometry({ 0.1f, 0.2f, -10 }, direction, 0, 5.999f).hit,
            "local/world direction scale incorrectly extended a finite segment");
        hit = scene.TraceGeometry({ 0.1f, 0.2f, -10 }, direction, 0, 6);
        Check(hit.hit && Near(hit.distance, 6), "local/world interval conversion changed world distance");

        Scene::LocalRay localRay;
        const DirectX::XMFLOAT3 conversionOrigin = { 0.1f, 0.2f, -10 };
        Check(scene.MakeLocalRay(0, conversionOrigin, direction, 2, 6, localRay),
            "valid finite world ray could not be converted to model-local space");
        Check(Near(localRay.origin, { 1.0f / 3, 0.2f / 3, -2.5f }, 2e-6) &&
            Near(localRay.direction, { 0, 0, 1 }, 1e-7) && Near(localRay.distanceScale, 0.25, 1e-12) &&
            Near(localRay.minimumDistance, 0.5, 1e-12) && Near(localRay.maximumDistance, 1.5, 1e-12),
            "nonuniform local ray conversion changed origin, normalization, distance scale or interval");
        const auto worldMatrix = DirectX::XMLoadFloat4x4(&scene.GetInstances()[0].worldFromLocal);
        const auto ToWorld = [&](double distance)
        {
            const auto point = Advance(localRay.origin, localRay.direction, float(distance));
            DirectX::XMFLOAT3 worldPoint;
            DirectX::XMStoreFloat3(&worldPoint, DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&point), worldMatrix));
            return worldPoint;
        };
        Check(Near(ToWorld(0), conversionOrigin, 2e-6) &&
            Near(ToWorld(localRay.minimumDistance), Advance(conversionOrigin, { 0, 0, 1 }, 2), 2e-6) &&
            Near(ToWorld(localRay.maximumDistance), Advance(conversionOrigin, { 0, 0, 1 }, 6), 2e-6),
            "local finite ray endpoints do not round-trip to the original world segment");
        Check(!scene.MakeLocalRay(UINT32_MAX, conversionOrigin, direction, 2, 6, localRay) &&
            !scene.MakeLocalRay(0, conversionOrigin, {}, 2, 6, localRay) &&
            !scene.MakeLocalRay(0, conversionOrigin, direction, -1, 6, localRay) &&
            !scene.MakeLocalRay(0, conversionOrigin, direction, 6, 2, localRay) &&
            !scene.MakeLocalRay(0, conversionOrigin, direction, 0, std::numeric_limits<float>::infinity(), localRay) &&
            !scene.MakeLocalRay(0, conversionOrigin, direction, std::numeric_limits<float>::quiet_NaN(), 6, localRay) &&
            !scene.MakeLocalRay(0, { std::numeric_limits<float>::quiet_NaN(), 0, 0 }, direction, 0, 6, localRay) &&
            !scene.MakeLocalRay(0, conversionOrigin, { 0, 0, std::numeric_limits<float>::infinity() }, 0, 6, localRay),
            "local ray conversion accepted invalid instance, origin, direction or finite world interval");

        // Float local-hit distance must not round a valid world hit past the
        // original finite endpoint. The transformed first plane is at
        // 1.2999999523162842, so its true world T is 0.8999999463558197.
        auto endpointModel = Plane(1);
        endpointModel.meshes.push_back(Plane(2).meshes.front());
        VXGIMeshSDFCache endpointCache;
        Scene endpointScene;
        std::vector<Instance> endpointInstances = { MakeInstance(endpointModel, 52, {}, { 1, 1, 1.3f }) };
        Check(endpointScene.UpdateInstances(endpointInstances, endpointCache, BuildSettings), endpointScene.GetLastError().c_str());
        constexpr float finiteEnd = 0.8999999761581421f;
        hit = endpointScene.TraceGeometry({ 0.13f, 0.17f, 0.4f }, { 0, 0, 1 }, 0, finiteEnd);
        Check(hit.hit && hit.sourceTriangleIndex < 2 && hit.meshIndex == 0 && hit.distance <= finiteEnd &&
            Near(hit.distance, 0.8999999463558197, 6e-8),
            "local float hit conversion rejected a true hit inside the original finite world segment");
        Check(!endpointScene.TraceGeometry({ 0.13f, 0.17f, 0.4f }, { 0, 0, 1 }, 0, 0.8999f).hit,
            "world endpoint precision fix extended the original finite segment");
        hit = endpointScene.TraceGeometry({ 0.13f, 0.17f, 0.4f }, { 0, 0, 1 }, 0.90001f, 2.2f);
        Check(hit.hit && hit.meshIndex == 1 && Near(hit.distance, 2.199999898672104, 2e-7),
            "rayMin past the near plane did not preserve the farther in-range surface");

        auto exactPointModel = Plane(1);
        VXGIMeshSDFCache exactPointCache;
        Scene exactPointScene;
        std::vector<Instance> exactPointInstances = { MakeInstance(exactPointModel, 61, { 0, 0, 1 }, { 1, 1, 0.7f }) };
        Check(exactPointScene.UpdateInstances(exactPointInstances, exactPointCache, BuildSettings), exactPointScene.GetLastError().c_str());
        constexpr float exactPoint = 1.7000000476837158f;
        const auto pointHit = exactPointScene.TraceGeometry({ 0.13f, 0.17f, 0 }, { 0, 0, 1 }, exactPoint, exactPoint);
        const bool exactPointPassed = pointHit.hit && pointHit.instanceId == 61 && pointHit.distance == exactPoint;

        auto continuationModel = Plane(1);
        continuationModel.meshes.push_back(Plane(2).meshes.front());
        VXGIMeshSDFCache continuationCache;
        Scene continuationScene;
        std::vector<Instance> continuationInstances = {
            MakeInstance(continuationModel, 62, { 0, 0, 0.1f }, { 1, 1, 0.3f }) };
        Check(continuationScene.UpdateInstances(continuationInstances, continuationCache, BuildSettings), continuationScene.GetLastError().c_str());
        const auto continuedHit = continuationScene.TraceGeometry({ 0.13f, 0.17f, 0.3f }, { 0, 0, 1 }, 0.1f, 1);
        const bool continuationPassed = continuedHit.hit && continuedHit.instanceId == 62 && continuedHit.meshIndex == 1 &&
            Near(continuedHit.distance, 0.4000000357627869, 1e-7);
        if (!exactPointPassed || !continuationPassed)
            std::cerr << "Finite candidate regressions: exact-point hit=" << pointHit.hit << " distance=" << pointHit.distance
                << "; continuation hit=" << continuedHit.hit << " mesh=" << continuedHit.meshIndex
                << " distance=" << continuedHit.distance << '\n';
        Check(exactPointPassed && continuationPassed,
            "world-valid exact endpoint was omitted or rejected local first hit prevented the farther valid surface");
    }
    void TestNearestAcrossOverlappingBounds()
    {
        Mesh farMesh;
        Quad(farMesh, { 3, -1, 1 }, { 4, -1, 1 }, { 4, 1, 1 }, { 3, 1, 1 });
        Quad(farMesh, { -1, -1, 8 }, { 1, -1, 8 }, { 1, 1, 8 }, { -1, 1, 8 });
        Model earlyBoundsLateSurface = { { std::move(farMesh) } };
        auto nearerSurface = Plane(4);
        std::vector<Instance> instances = { MakeInstance(earlyBoundsLateSurface, 1), MakeInstance(nearerSurface, 2) };
        VXGIMeshSDFCache cache;
        Scene scene;
        Check(scene.UpdateInstances(instances, cache, BuildSettings), scene.GetLastError().c_str());
        const auto hit = scene.TraceGeometry({ 0.13f, 0.17f, 0 }, { 0, 0, 1 }, 0, 10);
        Check(hit.hit && hit.instanceId == 2 && Near(hit.distance, 4),
            "first instance/AABB hit was returned instead of the closest actual surface");
        std::reverse(instances.begin(), instances.end());
        Check(scene.UpdateInstances(instances, cache, BuildSettings), scene.GetLastError().c_str());
        const auto reordered = scene.TraceGeometry({ 0.13f, 0.17f, 0 }, { 0, 0, 1 }, 0, 10);
        Check(reordered.hit && reordered.instanceId == 2 && reordered.sourceInstanceIndex == 0 &&
            Near(reordered.distance, 4), "source reordering changed the closest world hit");

        auto precisionFar = Plane(1.0001f), precisionNear = Plane(1);
        std::vector<Instance> precisionInstances = { MakeInstance(precisionFar, 1), MakeInstance(precisionNear, 2) };
        VXGIMeshSDFCache precisionCache;
        Scene precisionScene;
        Check(precisionScene.UpdateInstances(precisionInstances, precisionCache, BuildSettings), precisionScene.GetLastError().c_str());
        VXGIWorld precisionWorld;
        VXGISDF precisionReference;
        BuildReference(precisionInstances, precisionWorld, precisionReference);
        const auto expected = precisionReference.TraceReference({ 0.13f, 0.17f, -10000 }, { 0, 0, 1 }, 10002);
        const auto actual = precisionScene.TraceGeometry({ 0.13f, 0.17f, -10000 }, { 0, 0, 1 }, 0, 10002);
        // Both distances can round to the same float; nearest selection must
        // retain the double world parameter until after choosing the instance.
        Check(expected.hit && expected.instanceId == 2 && actual.hit && actual.instanceId == expected.instanceId,
            "rounded float distances selected the farther instance instead of the precise world-BVH hit");
    }
    void TestAutomaticDimensionsAndTransformReuse()
    {
        Mesh mesh;
        Quad(mesh, {-25, 0, -0.125f}, {-25, 0, 0.125f},
            {25, 0, 0.125f}, {25, 0, -0.125f});
        Model model = {{std::move(mesh)}};
        Settings settings;
        settings.cellSize = 0.125f;
        std::vector<Instance> instances = {MakeInstance(model, 71),
            MakeInstance(model, 72, {100, 0, 0})};
        VXGIMeshSDFCache cache;
        Scene scene;
        Check(scene.UpdateInstances(instances, cache, settings), scene.GetLastError().c_str());
        const auto asset = scene.GetInstances()[0].asset;
        Check(scene.GetInstances()[1].asset == asset && cache.GetStats().builds == 1 &&
            asset->GetSDF().GetGrid().nodeDimensions == std::array<uint32_t,3>{403, 3, 5} &&
            asset->GetSDF().GetGrid().cellSize == 0.125f,
            "instance scene rejected or coarsened an automatically sized long thin asset");
        const auto* nodes = asset->GetSDF().GetNodes().data();
        const auto revision = scene.GetRevision();
        Check(scene.UpdateInstances(instances, cache, settings) &&
            scene.GetRevision() == revision && scene.GetStats().transformUpdates == 0,
            "unchanged automatic instance scene did not stay idle");
        instances[0].translation = {3, 2, -7};
        instances[0].rotation = {0.2f, -0.4f, 0.1f};
        instances[0].scale = {-2, 0.5f, 3};
        Check(scene.UpdateInstances(instances, cache, settings) &&
            scene.GetStats().transformUpdates == 1 && cache.GetStats().builds == 1 &&
            scene.GetInstances()[0].asset == asset && scene.GetInstances()[1].asset == asset &&
            asset->GetSDF().GetNodes().data() == nodes,
            "moving or scaling an automatic instance rebuilt shared local distance data");
        const auto retainedRevision = scene.GetRevision();
        Settings tooFine;
        tooFine.cellSize = 0.0001f;
        Check(!scene.UpdateInstances(instances, cache, tooFine) &&
            scene.GetLastError().find("total grid budget of 16777216 nodes") != std::string::npos &&
            scene.GetRevision() == retainedRevision && cache.GetStats().builds == 1 &&
            scene.GetInstances()[0].asset == asset && asset->GetSDF().GetNodes().data() == nodes,
            "rejected automatic node budget did not retain the prior usable instance scene");
        Check(scene.UpdateInstances(instances, cache, settings) && scene.GetStats().transformUpdates == 0,
            "automatic instance scene did not recover after rejecting a fine field");
    }
    void TestCoverageIdentityAndRefresh()
    {
        auto model = Box();
        std::vector<Instance> instances = { MakeInstance(model, 11), MakeInstance(model, 22, { 4, 0, 0 }),
            MakeInstance(model, 33, { 0, 0, -4 }) };
        instances[0].isVisible = false;
        instances[0].currentLodLevel = 19;
        instances[0].isCutout = true;
        instances[1].customMaterialID = 97;
        instances[2].isTransparent = true;
        VXGIMeshSDFCache cache;
        Scene scene;
        Check(scene.UpdateInstances(instances, cache, BuildSettings), scene.GetLastError().c_str());
        Check(scene.GetStats().sceneInstances == 3 && scene.GetStats().includedInstances == 2 &&
            scene.GetStats().transparentInstances == 1 && scene.GetInstances().size() == 2,
            "transparent filtering or hidden/render-LOD participation is incorrect");
        auto hit = scene.TraceGeometry({ 0.13f, 0.17f, -10 }, { 0, 0, 1 }, 0, 15);
        // Geometry-only query deliberately does not claim texture alpha coverage.
        Check(hit.hit && hit.instanceId == 11 && hit.isCutout && hit.meshIndex == 0 && hit.materialId == 17,
            "hidden LOD0 cutout instance lost its geometry/material identity");
        hit = scene.TraceGeometry({ 4.13f, 0.17f, -10 }, { 0, 0, 1 }, 0, 15);
        Check(hit.hit && hit.instanceId == 22 && hit.materialId == 97 && !hit.isCutout,
            "instance material override was not resolved at a geometry hit");
        hit = scene.TraceGeometry({ -4, 0.13f, 0.17f }, { 1, 0, 0 }, 0, 8);
        Check(hit.hit && hit.instanceId == 11 && hit.meshIndex == 1 && hit.materialId == 31,
            "material slots were not resolved by source submesh");
        const auto revision = scene.GetRevision();
        const auto asset = scene.GetInstances()[0].asset;
        Check(scene.UpdateInstances(instances, cache, BuildSettings) && scene.GetRevision() == revision &&
            scene.GetStats().transformUpdates == 0, "unchanged scene update rebuilt instance transforms");
        const auto builds = cache.GetStats().builds;
        instances[0].translation = { 0, 2, 0 };
        instances[0].rotation = { 0.2f, 0.4f, 0.1f };
        instances[0].scale = { -1, 0.7f, 2 };
        instances[0].customMaterialID = 121;
        instances[0].isCutout = false;
        Check(scene.UpdateInstances(instances, cache, BuildSettings) && scene.GetRevision() > revision &&
            scene.GetStats().transformUpdates == 1 && cache.GetStats().builds == builds &&
            scene.GetInstances()[0].asset == asset && scene.GetInstances()[1].asset == asset &&
            scene.GetInstances()[0].materialIds == std::vector<uint32_t>({ 121, 121 }) &&
            !scene.GetInstances()[0].isCutout, "TRS/material refresh rebuilt or contaminated the shared asset");
        std::reverse(instances.begin(), instances.end());
        Check(scene.UpdateInstances(instances, cache, BuildSettings) && scene.GetStats().transformUpdates == 0 &&
            scene.GetInstances()[0].sourceInstanceIndex == 2 && cache.GetStats().builds == builds,
            "source reorder rebuilt unchanged transforms or lost source indices");
    }
    void TestTransformsAndRandomWorldReference()
    {
        auto first = Box(), second = Box();
        std::vector<Instance> instances = { MakeInstance(first, 41, { -2, 0.3f, 1.2f }, { -2, 0.5f, 1.5f }, { 0.1f, 0.2f, 0.3f }),
            MakeInstance(first, 19, { 3, 1, -2 }, { 0.7f, -1.3f, 0.8f }, { -0.3f, 0.6f, 0.2f }),
            MakeInstance(second, 63, { 0, -3, 2 }, { -0.4f, -1.8f, 1.1f }, { 0.4f, -0.2f, 0.7f }) };
        instances[0].customMaterialID = 91;
        instances[1].isCutout = true;
        VXGIMeshSDFCache cache;
        Scene scene;
        Check(scene.UpdateInstances(instances, cache, BuildSettings), scene.GetLastError().c_str());
        VXGIWorld world;
        VXGISDF reference;
        BuildReference(instances, world, reference);
        const DirectX::XMFLOAT3 worldOrigin = { 1.37f, -2.91f, 4.13f };
        const DirectX::XMFLOAT3 worldDirection = { 0.37f, -0.81f, 0.45f };
        const auto unitWorldDirection = Normalize(worldDirection);
        constexpr float worldMinimum = 0.17f, worldMaximum = 3.7f;
        for (size_t index = 0; index < scene.GetInstances().size(); ++index)
        {
            Scene::LocalRay localRay;
            Check(scene.MakeLocalRay(static_cast<uint32_t>(index), worldOrigin, worldDirection,
                worldMinimum, worldMaximum, localRay), "rotated mirrored instance rejected a valid local ray conversion");
            const double localLength = std::sqrt(double(localRay.direction.x) * localRay.direction.x +
                double(localRay.direction.y) * localRay.direction.y + double(localRay.direction.z) * localRay.direction.z);
            Check(Near(localLength, 1) && std::isfinite(localRay.distanceScale) && localRay.distanceScale > 0,
                "rotated mirrored local ray has a nonunit direction or invalid distance scale");
            const auto matrix = DirectX::XMLoadFloat4x4(&scene.GetInstances()[index].worldFromLocal);
            const auto ToWorld = [&](double localDistance)
            {
                const auto point = Advance(localRay.origin, localRay.direction, float(localDistance));
                DirectX::XMFLOAT3 result;
                DirectX::XMStoreFloat3(&result, DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&point), matrix));
                return result;
            };
            Check(Near(ToWorld(0), worldOrigin) &&
                Near(ToWorld(localRay.minimumDistance), Advance(worldOrigin, unitWorldDirection, worldMinimum)) &&
                Near(ToWorld(localRay.maximumDistance), Advance(worldOrigin, unitWorldDirection, worldMaximum)),
                "rotated mirrored local origin/interval endpoints do not round-trip to the original finite world ray");
        }
        // Aim at the interior of every transformed triangle to exercise front and
        // back faces, including one and two negative scale axes.
        for (const auto& triangle : world.GetTriangles())
        {
            const DirectX::XMFLOAT3 target = {
                triangle.positions[0].x * 0.2f + triangle.positions[1].x * 0.3f + triangle.positions[2].x * 0.5f,
                triangle.positions[0].y * 0.2f + triangle.positions[1].y * 0.3f + triangle.positions[2].y * 0.5f,
                triangle.positions[0].z * 0.2f + triangle.positions[1].z * 0.3f + triangle.positions[2].z * 0.5f };
            const auto normal = triangle.geometricNormal;
            Compare(scene, world, reference, Advance(target, normal, 0.1f), { -normal.x, -normal.y, -normal.z }, 0, 0.2f);
            Compare(scene, world, reference, Advance(target, normal, -0.1f), normal, 0, 0.2f);
        }
        std::mt19937 random(8031);
        std::uniform_real_distribution<float> position(-7, 7), component(-1, 1), minimum(0, 1.5f), length(0.01f, 18);
        for (size_t i = 0; i < 1000; ++i)
        {
            const DirectX::XMFLOAT3 origin = { position(random), position(random), position(random) };
            DirectX::XMFLOAT3 direction = { component(random), component(random), component(random) };
            if (std::abs(direction.x) + std::abs(direction.y) + std::abs(direction.z) < 0.001f) direction.z = 1;
            const float start = minimum(random);
            Compare(scene, world, reference, origin, direction, start, start + length(random));
        }
    }
    void TestInstanceBVHCandidatesAndUpdates()
    {
        auto model = Box();
        VXGIMeshSDFCache cache;
        Scene scene;
        std::vector<Instance> instances = { MakeInstance(model, 1, {0,0,8}),
            MakeInstance(model, 20, {0,0,2}), MakeInstance(model, 10, {0,0,5}) };
        Check(scene.UpdateInstances(instances, cache, BuildSettings), scene.GetLastError().c_str());
        const auto& bvh = scene.GetInstanceBVH();
        const auto bvhRevision = bvh.GetRevision();
        const auto bakes = cache.GetStats().builds;
        const auto* nodes = bvh.GetNodes().data();
        const auto CheckCandidates = [&](uint32_t first, uint32_t second)
        {
            const auto query = scene.QueryClosestTwoInstances({0,0,0}, {0,0,2}, 0, 20);
            Check(query.valid && query.count == 2 && query.candidates[0].instanceId == first &&
                query.candidates[1].instanceId == second &&
                query.candidates[0].entryDistance <= query.candidates[1].entryDistance,
                "scene BVH did not select the two nearest AABB entries");
            for (uint32_t i = 0; i < query.count; ++i)
                Check(scene.GetInstances()[query.candidates[i].instanceIndex].instanceId == query.candidates[i].instanceId,
                    "scene BVH candidate no longer resolves to its compact instance");
        };
        CheckCandidates(20, 10);
        Check(bvh.GetNodes().size() == 5 && bvh.GetStats().builds == 1,
            "scene did not build one instance BVH");
        Check(scene.UpdateInstances(instances, cache, BuildSettings), scene.GetLastError().c_str());
        Check(bvh.GetRevision() == bvhRevision && bvh.GetNodes().data() == nodes,
            "unchanged scene rebuilt the instance BVH");
        std::reverse(instances.begin(), instances.end());
        instances[0].customMaterialID = 99;
        instances[0].isCutout = true;
        Check(scene.UpdateInstances(instances, cache, BuildSettings), scene.GetLastError().c_str());
        Check(bvh.GetRevision() == bvhRevision && bvh.GetNodes().data() == nodes &&
            bvh.GetStats().refits == 0 && scene.GetStats().transformUpdates == 0,
            "source sorting or material metadata changed instance bounds/BVH");
        CheckCandidates(20, 10);
        const auto moved = std::find_if(instances.begin(), instances.end(),
            [](const auto& instance) { return instance.editorId == 20; });
        moved->translation.z = 12;
        Check(scene.UpdateInstances(instances, cache, BuildSettings), scene.GetLastError().c_str());
        CheckCandidates(10, 1);
        Check(bvh.GetStats().builds == 1 && bvh.GetStats().refits == 1 && cache.GetStats().builds == bakes,
            "TRS change rebuilt BVH topology or rebaked a local asset");
        instances.erase(std::remove_if(instances.begin(), instances.end(),
            [](const auto& instance) { return instance.editorId == 10; }), instances.end());
        Check(scene.UpdateInstances(instances, cache, BuildSettings), scene.GetLastError().c_str());
        CheckCandidates(1, 20);
        Check(bvh.GetStats().builds == 2 && bvh.GetNodes().size() == 3,
            "instance removal retained stale BVH leaves");
        instances[0].isTransparent = true;
        Check(scene.UpdateInstances(instances, cache, BuildSettings), scene.GetLastError().c_str());
        const auto query = scene.QueryClosestTwoInstances({0,0,0}, {0,0,1}, 0, 20);
        Check(query.valid && query.count == 1 && bvh.GetNodes().size() == 1 &&
            scene.GetInstances()[query.candidates[0].instanceIndex].instanceId == query.candidates[0].instanceId &&
            cache.GetStats().builds == bakes,
            "transparent filtering retained an invalid compact BVH index or rebaked an asset");
    }

    Model HollowFrame()
    {
        Mesh mesh;
        Quad(mesh, {-6,-6,0}, {-4,-6,0}, {-4,6,0}, {-6,6,0});
        Quad(mesh, {4,-6,0}, {6,-6,0}, {6,6,0}, {4,6,0});
        Quad(mesh, {-4,-6,0}, {4,-6,0}, {4,-4,0}, {-4,-4,0});
        Quad(mesh, {-4,4,0}, {4,4,0}, {4,6,0}, {-4,6,0});
        return {{std::move(mesh)}};
    }

    void TestThresholdSceneBudgetAndFiniteSupport()
    {
        using Query = VXGIMeshSDFThreshold;
        using Status = Query::Status;
        auto frame = HollowFrame();
        auto plane = Plane(0);
        VXGIMeshSDFCache cache;
        Scene scene;
        std::vector<Instance> instances = {MakeInstance(frame, 10, {0,0,2}),
            MakeInstance(frame, 20, {0,0,4}), MakeInstance(plane, 30, {0,0,6})};
        Check(scene.UpdateInstances(instances, cache, {.25f,128}), scene.GetLastError().c_str());
        Query::Stats stats;
        const auto omitted = Query::Trace(scene, {0,0,0}, {0,0,3}, 0, 10, &stats);
        Check(omitted.status == Status::Miss && stats.candidates == 2 && stats.localQueries == 2 &&
            scene.TraceGeometry({0,0,0}, {0,0,1}, 0, 10).hit,
            "threshold query silently searched a third model beyond the two-box budget");
        instances.erase(instances.begin() + 1);
        Check(scene.UpdateInstances(instances, cache, {.25f,128}), scene.GetLastError().c_str());
        const auto hit = Query::Trace(scene, {0,0,0}, {0,0,7}, 0, 10, &stats);
        Check(hit.status == Status::Hit && hit.instanceId == 30 && Near(hit.distance, 6) &&
            Near(hit.position, {0,0,hit.distance}) && stats.localQueries == 2 && stats.samples >= 2,
            "empty nearest model box prevented testing the second distance field");
        Check(Query::Trace(scene, {0,0,0}, {0,0,1}, 0, 5.99f).status == Status::Miss,
            "threshold shell extended a finite ray or expanded the tight model AABB");
        Check(Query::Trace(scene, {0,0,0}, {0,0,1}, 6, 6).status == Status::Hit,
            "zero-width candidate interval did not sample its surface");
        Check(Query::Trace(scene, {0,0,0}, {0,0,1}, 6.001f, 10).status == Status::Miss,
            "threshold query ignored rayMin and accepted a past surface");
        Check(Query::Trace(scene, {20,0,0}, {0,0,1}, 0, 10, &stats).status == Status::Miss &&
            stats.candidates == 0 && stats.localQueries == 0 && stats.samples == 0,
            "AABB miss unnecessarily sampled a local distance field");
        Query::Parameters invalid;
        invalid.hitThresholdCells = .1f;
        Check(Query::Trace(scene, {20,0,0}, {0,0,1}, 0, 10, invalid).status == Status::Invalid &&
            Query::Trace(scene, {}, {}, 0, 10).status == Status::Invalid,
            "empty broad phase bypassed invalid threshold/ray validation");
        instances.clear();
        Check(scene.UpdateInstances(instances, cache, {.25f,128}), scene.GetLastError().c_str());
        Check(Query::Trace(scene, {}, {0,0,1}, 0, 10).status == Status::Miss,
            "empty valid threshold scene did not return a bounded miss");
    }

    void TestThresholdSceneTransformsAndNearest()
    {
        using Query = VXGIMeshSDFThreshold;
        using Status = Query::Status;
        auto plane = Plane(.25f);
        VXGIMeshSDFCache cache;
        Scene scene;
        const std::array<DirectX::XMFLOAT3,3> scales = {{{1,1,1}, {2,.5f,3}, {-2,.5f,-3}}};
        for (const auto scale : scales)
        {
            const std::vector<Instance> instances = {MakeInstance(plane, 51, {2,-3,4}, scale, {.3f,-.6f,.2f})};
            Check(scene.UpdateInstances(instances, cache, {.25f,128}), scene.GetLastError().c_str());
            const auto& instance = scene.GetInstances()[0];
            const auto matrix = DirectX::XMLoadFloat4x4(&instance.worldFromLocal);
            DirectX::XMFLOAT3 origin, direction;
            const DirectX::XMFLOAT3 localOrigin = {0,0,-2}, localDirection = {0,0,1};
            DirectX::XMStoreFloat3(&origin, DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&localOrigin), matrix));
            DirectX::XMStoreFloat3(&direction, DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(&localDirection), matrix));
            const auto result = Query::Trace(scene, origin, direction, 0, 20);
            DirectX::XMFLOAT3 localHit;
            DirectX::XMStoreFloat3(&localHit, DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&result.position),
                DirectX::XMLoadFloat4x4(&instance.localFromWorld)));
            Check(result.status == Status::Hit && result.instanceId == 51 &&
                std::abs(localHit.z - .25f) <= .23f && result.distance >= 0 && result.distance <= 20 &&
                Near(result.position, Advance(origin, Normalize(direction), result.distance), 1e-5),
                "threshold scene lost original ray or local/world scale under transformed instance");
        }
        auto flat = Plane(0);
        const std::vector<Instance> instances = {MakeInstance(flat, 1, {0,0,6}, {1,1,.2f}),
            MakeInstance(flat, 2, {0,0,4}, {1,1,4})};
        Check(scene.UpdateInstances(instances, cache, {.25f,128}), scene.GetLastError().c_str());
        const auto nearest = Query::Trace(scene, {}, {0,0,1}, 0, 10);
        Check(nearest.status == Status::Hit && nearest.instanceId == 2 && Near(nearest.distance,4),
            "threshold scene compared local distances across differently scaled models");

        // First AABB starts earlier, but its only central surface is farther.
        Mesh delayedMesh;
        Quad(delayedMesh, {5,-1,1}, {6,-1,1}, {6,1,1}, {5,1,1});
        Quad(delayedMesh, {-1,-1,8}, {1,-1,8}, {1,1,8}, {-1,1,8});
        Model delayed = {{std::move(delayedMesh)}};
        const std::vector<Instance> overlapping = {MakeInstance(delayed, 10), MakeInstance(flat, 20, {0,0,4})};
        Check(scene.UpdateInstances(overlapping, cache, {.25f,128}), scene.GetLastError().c_str());
        Query::Stats stats;
        const auto across = Query::Trace(scene, {}, {0,0,1}, 0, 10, &stats);
        Check(across.status == Status::Hit && across.instanceId == 20 && Near(across.distance,4) && stats.localQueries == 2,
            "threshold query treated nearest AABB entry as nearest approximate surface");
        Query::Parameters cap;
        cap.maxSteps = 1;
        const auto incomplete = Query::Trace(scene, {}, {0,0,1}, 0, 10, cap);
        Check(incomplete.status == Status::Exhausted && incomplete.instanceIndex == UINT32_MAX,
            "unfinished nearer candidate was reported as a proven farther hit");
    }

    void TestFailedSceneUpdatesAreTransactional()
    {
        auto model = Box();
        Model empty;
        std::vector<Instance> instances = { MakeInstance(model, 41) };
        VXGIMeshSDFCache cache;
        Scene scene;
        Check(scene.UpdateInstances(instances, cache, BuildSettings), scene.GetLastError().c_str());
        const auto revision = scene.GetRevision();
        const auto bvhRevision = scene.GetInstanceBVH().GetRevision();
        const auto asset = scene.GetInstances()[0].asset;
        const auto CheckRetained = [&]
        {
            const auto hit = scene.TraceGeometry({ 0.13f, 0.17f, -3 }, { 0, 0, 1 }, 0, 4);
            Check(scene.GetRevision() == revision && scene.GetInstances().size() == 1 &&
                scene.GetInstances()[0].asset == asset && hit.hit && hit.instanceId == 41 && Near(hit.distance, 2),
                "failed scene update replaced the previously valid scene");
            const auto candidates = scene.QueryClosestTwoInstances({0.13f,0.17f,-3}, {0,0,1}, 0, 4);
            Check(scene.GetInstanceBVH().GetRevision() == bvhRevision && candidates.valid && candidates.count == 1 &&
                candidates.candidates[0].instanceId == 41, "failed scene update replaced the previous instance BVH");
        };
        auto invalid = instances;
        invalid[0].scale.x = 0;
        Check(!scene.UpdateInstances(invalid, cache, BuildSettings) && !scene.GetLastError().empty(), "zero scale was accepted");
        CheckRetained();
        invalid = instances;
        invalid[0].translation.z = std::numeric_limits<float>::quiet_NaN();
        Check(!scene.UpdateInstances(invalid, cache, BuildSettings) && !scene.GetLastError().empty(), "nonfinite transform was accepted");
        CheckRetained();
        const std::vector<Instance> noInstances;
        Check(!scene.UpdateInstances(noInstances, cache, { 0, 128 }) && !scene.GetLastError().empty(),
            "empty input bypassed invalid build settings");
        CheckRetained();
        Check(!scene.UpdateInstances(instances, cache, { 0.01f, 3 }) && !scene.GetLastError().empty(),
            "failed local asset bake was accepted as a scene update");
        CheckRetained();
        Check(scene.UpdateInstances(instances, cache, BuildSettings) && scene.GetRevision() == revision &&
            scene.GetLastError().empty(), "valid unchanged update did not recover after an error");
        instances[0].isTransparent = true;
        Check(scene.UpdateInstances(instances, cache, BuildSettings) && scene.GetInstances().empty() &&
            scene.GetStats().transparentInstances == 1 &&
            !scene.TraceGeometry({ 0, 0, -3 }, { 0, 0, 1 }, 0, 10).hit,
            "all-transparent input did not produce an empty geometry scene");
        instances = { MakeInstance(empty, 99), Instance{} };
        Check(scene.UpdateInstances(instances, cache, BuildSettings) && scene.GetInstances().empty() &&
            scene.GetStats().emptyInstances == 2, "empty/null models did not produce an empty geometry scene");
        Check(scene.UpdateInstances(noInstances, cache, BuildSettings) && scene.GetInstances().empty(),
            "empty scene update failed");
    }
}

int main()
{
    try
    {
        TestWorldBoundsAndFiltering();
        TestFloorRejectsOutgoingRays();
        TestFiniteWorldIntervalsAndStarts();
        TestNearestAcrossOverlappingBounds();
        TestCoverageIdentityAndRefresh();
        TestAutomaticDimensionsAndTransformReuse();
        TestTransformsAndRandomWorldReference();
        TestInstanceBVHCandidatesAndUpdates();
        TestFailedSceneUpdatesAreTransactional();
        TestThresholdSceneBudgetAndFiniteSupport();
        TestThresholdSceneTransformsAndNearest();
        std::cout << "VXGIMeshSDFScene: all 11 CPU test groups passed (1072 world-BVH comparisons, automatic asset dimensions and reuse, instance-BVH lifecycle and threshold top-two scene queries; no texture alpha coverage).\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "VXGIMeshSDFScene: " << error.what() << '\n';
        return 1;
    }
}
