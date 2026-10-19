#pragma once

#include "VXGIMeshSDF.h"
#include "VXGIInstanceBVH.h"

// Instance metadata over immutable local assets. This CPU geometry query is the
// reference for the later GPU migration; it does not evaluate alpha textures.
class VXGIMeshSDFScene
{
public:
    struct Instance
    {
        std::shared_ptr<const VXGIMeshSDFCache::Asset> asset;
        uint32_t instanceId = 0;
        uint32_t sourceInstanceIndex = 0;
        bool isCutout = false;
        std::vector<uint32_t> materialIds;
        DirectX::XMFLOAT3 translation = {}, rotation = {}, scale = {1, 1, 1};
        DirectX::XMFLOAT4X4 worldFromLocal = {}, localFromWorld = {}, normalFromLocal = {};
        DirectX::XMFLOAT3 boundsMinimum = {}, boundsMaximum = {};
    };

    struct Stats
    {
        uint64_t sceneInstances = 0;
        uint64_t includedInstances = 0;
        uint64_t transparentInstances = 0;
        uint64_t emptyInstances = 0;
        uint64_t transformUpdates = 0; // This UpdateInstances call, not asset bakes.
    };

    struct Hit
    {
        bool hit = false;
        float distance = 0;
        DirectX::XMFLOAT3 position = {}, geometricNormal = {}, barycentrics = {};
        uint32_t instanceIndex = UINT32_MAX; // Compact table index.
        uint32_t instanceId = 0, sourceInstanceIndex = UINT32_MAX;
        uint32_t triangleIndex = UINT32_MAX; // Local to the referenced asset.
        uint32_t meshIndex = UINT32_MAX, sourceTriangleIndex = UINT32_MAX;
        uint32_t materialId = UINT32_MAX;
        bool backFace = false, isCutout = false;
    };

    struct TraceStats
    {
        uint64_t boxTests = 0, boxHits = 0, localTraces = 0;
        uint64_t nodeTests = 0, triangleTests = 0;
    };

    struct LocalRay
    {
        DirectX::XMFLOAT3 origin = {}, direction = {};
        double distanceScale = 0;
        double minimumDistance = 0, maximumDistance = 0;
    };

    template<class InstanceRange>
    bool UpdateInstances(const InstanceRange& sources, VXGIMeshSDFCache& cache,
        const VXGIMeshSDFCache::BuildSettings& settings)
    {
        if (!std::isfinite(settings.cellSize) || settings.cellSize <= 0 ||
            (settings.maxAxisResolution != 0 &&
                (settings.maxAxisResolution < 3 || settings.maxAxisResolution > 256)))
        {
            m_error = "Mesh SDF instance scene requires a positive cell size and an automatic (0) or internal 3..256 node axis limit.";
            return false;
        }
        try
        {
            std::vector<size_t> order(sources.size());
            std::iota(order.begin(), order.end(), size_t(0));
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b)
                { return sources[a].editorId < sources[b].editorId; });
            std::vector<Instance> next;
            next.reserve(sources.size());
            Stats stats;
            stats.sceneInstances = sources.size();
            for (const size_t sourceIndex : order)
            {
                const auto& source = sources[sourceIndex];
                if (source.isTransparent) { ++stats.transparentInstances; continue; }
                if (!source.pModel || source.pModel->meshes.empty()) { ++stats.emptyInstances; continue; }
                if (!Finite(source.translation) || !Finite(source.rotation) || !Finite(source.scale) ||
                    source.scale.x == 0 || source.scale.y == 0 || source.scale.z == 0)
                {
                    m_error = "Mesh SDF instance transform must be finite and invertible.";
                    return false;
                }
                Instance instance;
                instance.asset = cache.GetOrCreate(*source.pModel, settings);
                if (!instance.asset) { m_error = cache.GetLastError(); return false; }
                if (!instance.asset->GetSDF().GetGrid().valid) { ++stats.emptyInstances; continue; }
                instance.instanceId = source.editorId;
                instance.sourceInstanceIndex = static_cast<uint32_t>(sourceIndex);
                instance.isCutout = source.isCutout;
                instance.translation = source.translation;
                instance.rotation = source.rotation;
                instance.scale = source.scale;
                instance.materialIds.reserve(source.pModel->meshes.size());
                for (const auto& mesh : source.pModel->meshes)
                    instance.materialIds.push_back(source.customMaterialID == UINT32_MAX ? mesh.materialID : source.customMaterialID);

                // Render sorting and material edits do not require recalculating
                // matrices or bounds, and never change the shared local asset.
                const Instance* previous = next.size() < m_instances.size() ? &m_instances[next.size()] : nullptr;
                if (previous && previous->instanceId == instance.instanceId && SameTransform(*previous, instance))
                {
                    instance.worldFromLocal = previous->worldFromLocal;
                    instance.localFromWorld = previous->localFromWorld;
                    instance.normalFromLocal = previous->normalFromLocal;
                    instance.boundsMinimum = previous->boundsMinimum;
                    instance.boundsMaximum = previous->boundsMaximum;
                }
                else
                {
                    if (!BuildTransform(instance)) return false;
                    ++stats.transformUpdates;
                }
                next.push_back(std::move(instance));
            }
            stats.includedInstances = next.size();
            bool changed = !m_initialized || !(m_settings == settings) || next.size() != m_instances.size();
            if (!changed)
                for (size_t i = 0; i < next.size(); ++i)
                    if (!SameInstance(next[i], m_instances[i])) { changed = true; break; }
            // Maintain the instance broad phase independently of materials and
            // probe history. Update is transactional; the remaining scene
            // commit moves an already prepared vector without allocation.
            if (!m_instanceBVH.Update(next))
            {
                m_error = "Mesh SDF instance BVH: " + m_instanceBVH.GetLastError();
                return false;
            }
            if (changed)
            {
                m_instances = std::move(next);
                m_settings = settings;
                m_initialized = true;
                ++m_revision;
            }
            m_stats = stats;
            m_error.clear();
            return true;
        }
        catch (const std::exception& exception)
        {
            m_error = std::string("Mesh SDF instance update failed: ") + exception.what();
            return false;
        }
    }

    // Keep the original origin and interval. Local normalized distance equals
    // world distance times distanceScale, including nonuniform signed scaling.
    bool MakeLocalRay(uint32_t instanceIndex, const DirectX::XMFLOAT3& origin,
        const DirectX::XMFLOAT3& direction, float rayMin, float rayMax, LocalRay& result) const
    {
        result = {};
        Vector unit;
        if (instanceIndex >= m_instances.size() || !NormalizeRay(origin, direction, rayMin, rayMax, unit)) return false;
        const DirectX::XMFLOAT3 worldDirection = {float(unit.x), float(unit.y), float(unit.z)};
        const auto inverse = DirectX::XMLoadFloat4x4(&m_instances[instanceIndex].localFromWorld);
        DirectX::XMFLOAT3 localVector;
        DirectX::XMStoreFloat3(&result.origin, DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&origin), inverse));
        DirectX::XMStoreFloat3(&localVector, DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(&worldDirection), inverse));
        result.distanceScale = std::sqrt(double(localVector.x) * localVector.x +
            double(localVector.y) * localVector.y + double(localVector.z) * localVector.z);
        if (!Finite(result.origin) || !Finite(localVector) || !std::isfinite(result.distanceScale) || result.distanceScale == 0)
        {
            result = {};
            return false;
        }
        result.direction = {float(localVector.x / result.distanceScale), float(localVector.y / result.distanceScale),
            float(localVector.z / result.distanceScale)};
        result.minimumDistance = rayMin * result.distanceScale;
        result.maximumDistance = rayMax * result.distanceScale;
        return true;
    }

    // Independent full-triangle CPU reference, not the production SDF marcher.
    // Walk the existing asset BVH with transformed conservative node bounds;
    // clip exact world triangles to the original world interval. This avoids
    // float inverse-ray endpoint errors hiding candidates before validation.
    // Alpha textures are deliberately not evaluated by this geometry reference.
    Hit TraceGeometry(const DirectX::XMFLOAT3& origin, const DirectX::XMFLOAT3& direction,
        float rayMin, float rayMax, TraceStats* traceStats = nullptr) const
    {
        if (traceStats) *traceStats = {};
        Hit best;
        Vector preciseDirection;
        if (!NormalizeRay(origin, direction, rayMin, rayMax, preciseDirection)) return best;
        double closest = rayMax;
        for (size_t instanceIndex = 0; instanceIndex < m_instances.size(); ++instanceIndex)
        {
            const auto& instance = m_instances[instanceIndex];
            if (traceStats) ++traceStats->boxTests;
            if (!RayBox(origin, preciseDirection,
                {instance.boundsMinimum.x, instance.boundsMinimum.y, instance.boundsMinimum.z},
                {instance.boundsMaximum.x, instance.boundsMaximum.y, instance.boundsMaximum.z}, rayMin, closest)) continue;
            if (traceStats) ++traceStats->boxHits;
            if (traceStats) ++traceStats->localTraces;
            const auto& geometry = instance.asset->GetSDF().m_data;
            std::array<uint32_t, 64> stack = {};
            size_t pending = 1;
            while (pending)
            {
                const auto& node = geometry.bvh[stack[--pending]];
                Vector minimum, maximum;
                TransformBounds(instance, {node.bounds.minimum.x, node.bounds.minimum.y, node.bounds.minimum.z},
                    {node.bounds.maximum.x, node.bounds.maximum.y, node.bounds.maximum.z}, minimum, maximum);
                if (traceStats) ++traceStats->nodeTests;
                if (!RayBox(origin, preciseDirection, minimum, maximum, rayMin, closest)) continue;
                if (!node.count)
                {
                    // The immutable median-split BVH has depth < 32.
                    stack[pending++] = node.right;
                    stack[pending++] = node.left;
                    continue;
                }
                for (uint32_t i = 0; i < node.count; ++i)
                {
                    const uint32_t triangleIndex = geometry.order[node.first + i];
                    const auto& triangle = geometry.triangles[triangleIndex];
                    double worldDistance;
                    DirectX::XMFLOAT3 normal, barycentrics;
                    if (traceStats) ++traceStats->triangleTests;
                    if (!IntersectWorldTriangle(instance, triangle, origin, preciseDirection,
                        rayMin, closest, worldDistance, normal, barycentrics)) continue;
                    if (best.hit && worldDistance == closest &&
                        (instanceIndex > best.instanceIndex || triangleIndex >= best.triangleIndex)) continue;
                    best.hit = true;
                    closest = worldDistance;
                    best.distance = static_cast<float>(worldDistance);
                    best.position = {float(double(origin.x) + preciseDirection.x * worldDistance),
                        float(double(origin.y) + preciseDirection.y * worldDistance),
                        float(double(origin.z) + preciseDirection.z * worldDistance)};
                    best.geometricNormal = normal;
                    best.barycentrics = barycentrics;
                    best.instanceIndex = static_cast<uint32_t>(instanceIndex);
                    best.instanceId = instance.instanceId;
                    best.sourceInstanceIndex = instance.sourceInstanceIndex;
                    best.triangleIndex = triangleIndex;
                    best.meshIndex = triangle.meshIndex;
                    best.sourceTriangleIndex = triangle.sourceTriangleIndex;
                    best.materialId = instance.materialIds[triangle.meshIndex];
                    best.isCutout = instance.isCutout;
                    best.backFace = preciseDirection.x * normal.x + preciseDirection.y * normal.y + preciseDirection.z * normal.z > 0;
                }
            }
        }
        return best;
    }

    const std::vector<Instance>& GetInstances() const { return m_instances; }
    const VXGIInstanceBVH& GetInstanceBVH() const { return m_instanceBVH; }
    VXGIInstanceBVH::QueryResult QueryClosestTwoInstances(const DirectX::XMFLOAT3& origin,
        const DirectX::XMFLOAT3& direction, float rayMin, float rayMax,
        VXGIInstanceBVH::QueryStats* queryStats = nullptr) const
    {
        return m_instanceBVH.QueryClosestTwo(origin, direction, rayMin, rayMax, queryStats);
    }
    const Stats& GetStats() const { return m_stats; }
    const std::string& GetLastError() const { return m_error; }
    uint64_t GetRevision() const { return m_revision; }

private:
    struct Vector { double x, y, z; };
    std::vector<Instance> m_instances;
    VXGIInstanceBVH m_instanceBVH;
    VXGIMeshSDFCache::BuildSettings m_settings;
    Stats m_stats;
    uint64_t m_revision = 0;
    bool m_initialized = false;
    std::string m_error;

    static bool Finite(DirectX::XMFLOAT3 value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }
    static bool NormalizeRay(DirectX::XMFLOAT3 origin, DirectX::XMFLOAT3 direction,
        float rayMin, float rayMax, Vector& unit)
    {
        if (!Finite(origin) || !Finite(direction) || !std::isfinite(rayMin) ||
            !std::isfinite(rayMax) || rayMin < 0 || rayMax < rayMin) return false;
        const double length = std::sqrt(double(direction.x) * direction.x +
            double(direction.y) * direction.y + double(direction.z) * direction.z);
        if (length == 0) return false;
        unit = {direction.x / length, direction.y / length, direction.z / length};
        return true;
    }
    static bool Same(DirectX::XMFLOAT3 a, DirectX::XMFLOAT3 b)
    {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
    static bool SameTransform(const Instance& a, const Instance& b)
    {
        return a.asset == b.asset && Same(a.translation, b.translation) &&
            Same(a.rotation, b.rotation) && Same(a.scale, b.scale);
    }
    static bool SameInstance(const Instance& a, const Instance& b)
    {
        return SameTransform(a, b) && a.instanceId == b.instanceId &&
            a.sourceInstanceIndex == b.sourceInstanceIndex && a.isCutout == b.isCutout && a.materialIds == b.materialIds;
    }

    bool BuildTransform(Instance& instance)
    {
        const auto world = DirectX::XMMatrixScaling(instance.scale.x, instance.scale.y, instance.scale.z) *
            DirectX::XMMatrixRotationRollPitchYaw(instance.rotation.x, instance.rotation.y, instance.rotation.z) *
            DirectX::XMMatrixTranslation(instance.translation.x, instance.translation.y, instance.translation.z);
        const auto inverse = DirectX::XMMatrixInverse(nullptr, world);
        if (DirectX::XMMatrixIsNaN(world) || DirectX::XMMatrixIsInfinite(world) ||
            DirectX::XMMatrixIsNaN(inverse) || DirectX::XMMatrixIsInfinite(inverse))
        {
            m_error = "Mesh SDF instance matrix exceeds finite invertible coordinates.";
            return false;
        }
        DirectX::XMStoreFloat4x4(&instance.worldFromLocal, world);
        DirectX::XMStoreFloat4x4(&instance.localFromWorld, inverse);
        DirectX::XMStoreFloat4x4(&instance.normalFromLocal, DirectX::XMMatrixTranspose(inverse));
        const auto& bounds = instance.asset->GetTraceBounds();
        Vector minimum, maximum;
        // The padded distance lattice is not the geometry envelope. In
        // particular, rays leaving a floor need not enter its empty SDF padding.
        TransformBounds(instance, {bounds.minimum.x, bounds.minimum.y, bounds.minimum.z},
            {bounds.maximum.x, bounds.maximum.y, bounds.maximum.z}, minimum, maximum);
        instance.boundsMinimum = {OutwardFloat(minimum.x, false), OutwardFloat(minimum.y, false), OutwardFloat(minimum.z, false)};
        instance.boundsMaximum = {OutwardFloat(maximum.x, true), OutwardFloat(maximum.y, true), OutwardFloat(maximum.z, true)};
        if (!Finite(instance.boundsMinimum) || !Finite(instance.boundsMaximum))
        {
            m_error = "Mesh SDF instance bounds exceed finite world coordinates.";
            return false;
        }
        return true;
    }

    static float OutwardFloat(double value, bool upper)
    {
        float rounded = static_cast<float>(value);
        if (upper ? double(rounded) < value : double(rounded) > value)
            rounded = std::nextafter(rounded, upper ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity());
        return rounded;
    }

    static void TransformBounds(const Instance& instance, Vector minimum, Vector maximum,
        Vector& worldMinimum, Vector& worldMaximum)
    {
        double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (uint32_t corner = 0; corner < 8; ++corner)
        {
            const double p[3] = {corner & 1 ? maximum.x : minimum.x,
                corner & 2 ? maximum.y : minimum.y, corner & 4 ? maximum.z : minimum.z};
            for (size_t axis = 0; axis < 3; ++axis)
            {
                double coordinate = instance.worldFromLocal.m[3][axis];
                double magnitude = std::abs(coordinate);
                for (size_t row = 0; row < 3; ++row)
                {
                    const double term = p[row] * instance.worldFromLocal.m[row][axis];
                    coordinate += term;
                    magnitude += std::abs(term);
                }
                // Bound the float XM vertex transform (also when fused). Only
                // candidate bounds expand; the world ray interval stays exact.
                const double margin = 8 * std::numeric_limits<float>::epsilon() * magnitude;
                lo[axis] = (std::min)(lo[axis], coordinate - margin);
                hi[axis] = (std::max)(hi[axis], coordinate + margin);
            }
        }
        worldMinimum = {lo[0], lo[1], lo[2]};
        worldMaximum = {hi[0], hi[1], hi[2]};
    }

    static bool RayBox(DirectX::XMFLOAT3 origin, Vector direction,
        Vector minimum, Vector maximum, double rayMin, double rayMax)
    {
        const double o[3] = {origin.x, origin.y, origin.z};
        const double d[3] = {direction.x, direction.y, direction.z};
        const double lo[3] = {minimum.x, minimum.y, minimum.z}, hi[3] = {maximum.x, maximum.y, maximum.z};
        for (size_t axis = 0; axis < 3; ++axis)
        {
            if (d[axis] == 0)
            {
                if (o[axis] < lo[axis] || o[axis] > hi[axis]) return false;
            }
            else
            {
                const double a = (lo[axis] - o[axis]) / d[axis], b = (hi[axis] - o[axis]) / d[axis];
                rayMin = (std::max)(rayMin, (std::min)(a, b));
                rayMax = (std::min)(rayMax, (std::max)(a, b));
                if (rayMin > rayMax) return false;
            }
        }
        return true;
    }

    static Vector Subtract(Vector a, Vector b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    static Vector Cross(Vector a, Vector b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
    static double Dot(Vector a, Vector b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

    static bool IntersectWorldTriangle(const Instance& instance, const VXGIWorld::Triangle& triangle,
        DirectX::XMFLOAT3 origin, Vector direction, double minimum, double maximum,
        double& distance, DirectX::XMFLOAT3& geometricNormal, DirectX::XMFLOAT3& barycentrics)
    {
        // Match the old world's transformed vertex winding, including mirrors.
        // Shading TBN will separately use normalFromLocal without this sign fix.
        const auto world = DirectX::XMLoadFloat4x4(&instance.worldFromLocal);
        DirectX::XMFLOAT3 p[3];
        for (size_t i = 0; i < 3; ++i)
            DirectX::XMStoreFloat3(&p[i], DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&triangle.positions[i]), world));
        const Vector a = {p[0].x, p[0].y, p[0].z};
        const Vector ab = Subtract({p[1].x, p[1].y, p[1].z}, a);
        const Vector ac = Subtract({p[2].x, p[2].y, p[2].z}, a);
        const Vector normal = Cross(ab, ac), cross = Cross(direction, ac);
        const double normalLength = std::sqrt(Dot(normal, normal));
        const double determinant = Dot(ab, cross);
        if (std::abs(determinant) <= 1e-12 * normalLength) return false;
        const double inverse = 1 / determinant;
        const Vector offset = Subtract({origin.x, origin.y, origin.z}, a);
        const double u = Dot(offset, cross) * inverse;
        const Vector q = Cross(offset, ab);
        const double v = Dot(direction, q) * inverse;
        if (u < -1e-10 || v < -1e-10 || u + v > 1 + 1e-10) return false;
        distance = Dot(ac, q) * inverse;
        if (distance < minimum || distance > maximum) return false;
        geometricNormal = {float(normal.x / normalLength), float(normal.y / normalLength), float(normal.z / normalLength)};
        barycentrics = {float(1 - u - v), float(u), float(v)};
        return true;
    }
};
