#pragma once

#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Stage 1 owns CPU world triangles and sparse allocation only. Neither allocation
// nor probe positions depend on the render-visible list, camera LOD or isDirty.
// Triangle retains the original surface identity and transformed vertex attributes
// so ray hits can reconstruct the same UV/TBN contract as the GBuffer.
class VXGIWorld
{
public:
    struct Coordinate
    {
        int32_t x = 0, y = 0, z = 0;
        bool operator==(const Coordinate& other) const
        {
            return x == other.x && y == other.y && z == other.z;
        }
        bool operator<(const Coordinate& other) const
        {
            if (x != other.x) return x < other.x;
            if (y != other.y) return y < other.y;
            return z < other.z;
        }
    };

    struct Grid
    {
        Coordinate minimum;
        Coordinate maximum;
        DirectX::XMFLOAT3 origin = {};
        DirectX::XMFLOAT3 boundsMaximum = {};
        bool valid = false;
    };

    struct Triangle
    {
        std::array<DirectX::XMFLOAT3, 3> positions;
        DirectX::XMFLOAT3 geometricNormal;
        std::array<DirectX::XMFLOAT2, 3> uv = {};
        std::array<DirectX::XMFLOAT3, 3> shadingNormals = {};
        std::array<DirectX::XMFLOAT3, 3> tangents = {};
        std::array<DirectX::XMFLOAT3, 3> bitangents = {};
        std::array<uint32_t, 3> sourceVertexIndices;
        uint32_t instanceId = 0;
        uint32_t sourceInstanceIndex = 0;
        uint32_t meshIndex = 0;
        uint32_t sourceTriangleIndex = 0;
        uint32_t materialId = 0;
        bool isCutout = false; // Full triangles for allocation/SDF; GPU rays evaluate hit-UV alpha coverage.
    };

    struct Cell
    {
        Coordinate coordinate;
        uint32_t firstTriangle = 0;
        uint32_t triangleCount = 0;
        // Compact index within the owning collection. GetCells() retains its
        // geometry index; GetProbeCells() owns the actual probe index.
        uint32_t probeIndex = 0;
    };

    enum class ProbeState : uint8_t { Unclassified, Valid, Disabled };
    struct Probe
    {
        DirectX::XMFLOAT3 position;
        std::array<DirectX::XMFLOAT3, 4> sh = {}; // Unconvolved RGB SH, l=0,1.
        ProbeState state = ProbeState::Unclassified;
    };

    struct Stats
    {
        uint64_t sceneInstances = 0;
        uint64_t includedInstances = 0;
        uint64_t transparentInstances = 0;
        uint64_t cutoutInstances = 0;
        uint64_t triangles = 0;
        uint64_t degenerateTriangles = 0;
        uint64_t invalidTriangles = 0;
        uint64_t cells = 0;
        uint64_t probeCells = 0;
        uint64_t haloProbeCells = 0;
        uint32_t probePaddingLayers = 0;
        uint64_t triangleReferences = 0;
        uint64_t candidateBoxTests = 0;
        uint64_t memoryBytes = 0; // Retained CPU capacity plus estimated hash nodes.
        double buildMilliseconds = 0.0;
        double materialRefreshMilliseconds = 0.0;
        bool geometryRebuilt = false; // Last committed update, not a per-frame flag.
        uint64_t geometryFingerprint = 0;
        uint64_t materialFingerprint = 0;
        uint64_t coverageFingerprint = 0; // Resolved material IDs and cutout flags.
    };

    // Template keeps this header independent of the asset-loading implementation
    // in PBR_Model.h. Call Update(resourceManager.GetSceneInstances(), voxelSize).
    // Unchanged transforms/material metadata avoid rebuilding. Explicitly call
    // Invalidate after editing vertex/index contents in place (static assets are
    // otherwise immutable). Transforms are reconstructed from TRS, not cachedMat.
    template<class InstanceRange>
    bool Update(const InstanceRange& allInstances, float voxelSize, uint32_t probePaddingLayers = 0)
    {
        if (!std::isfinite(voxelSize) || voxelSize <= 0.0f)
        {
            m_lastError = "VXGI voxelSize must be finite and greater than zero.";
            return false;
        }
        if (probePaddingLayers > 2)
        {
            m_lastError = "VXGI probe padding supports zero, one or two layers.";
            return false;
        }
        try
        {
            auto snapshot = CaptureSnapshot(allInstances);
            const bool snapshotUnchanged = snapshot == m_snapshot;
            if (!m_invalidated && m_initialized && voxelSize == m_voxelSize && snapshotUnchanged &&
                probePaddingLayers == m_storage.stats.probePaddingLayers)
            {
                m_lastError.clear();
                return true;
            }
            const auto start = std::chrono::steady_clock::now();
            const bool geometryChanged = m_invalidated || !m_initialized || !SameGeometry(snapshot, m_snapshot);
            const uint64_t materialFingerprint = MaterialFingerprint(snapshot);
            const bool materialChanged = !m_initialized || materialFingerprint != m_storage.stats.materialFingerprint;
            if (!geometryChanged && voxelSize == m_voxelSize)
            {
                // Padding changes only the probe allocation. Build it before
                // touching committed geometry/material metadata, and retain the
                // original SAT occupancy, triangle CSR and distance-field key.
                const bool paddingChanged = probePaddingLayers != m_storage.stats.probePaddingLayers;
                ProbeAllocation nextProbes;
                if (paddingChanged && !AllocateProbes(m_storage.cells, voxelSize, probePaddingLayers, nextProbes)) return false;
                if (!snapshotUnchanged) RefreshMaterials(allInstances, snapshot);
                const auto coverageFingerprint=CoverageFingerprint(m_storage.triangles);
                if(coverageFingerprint!=m_storage.stats.coverageFingerprint) ++m_coverageRevision;
                m_storage.stats.coverageFingerprint=coverageFingerprint;
                m_storage.stats.materialFingerprint = materialFingerprint;
                m_storage.stats.geometryRebuilt = false;
                if (paddingChanged)
                {
                    PublishProbes(m_storage, std::move(nextProbes), probePaddingLayers);
                    ++m_allocationRevision;
                }
                m_snapshot = std::move(snapshot);
                if (materialChanged) ++m_materialRevision;
                ++m_revision;
                m_storage.stats.memoryBytes = EstimateMemoryBytes();
                m_storage.stats.buildMilliseconds =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
                m_storage.stats.materialRefreshMilliseconds = snapshotUnchanged ? 0 : m_storage.stats.buildMilliseconds;
                m_lastError.clear();
                return true;
            }
            Storage next;
            next.stats.sceneInstances = allInstances.size();
            std::vector<size_t> order(allInstances.size());
            for (size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                return allInstances[a].editorId < allInstances[b].editorId;
            });
            uint64_t fingerprint = HashSeed;
            for (size_t sourceIndex : order)
            {
                const auto& instance = allInstances[sourceIndex];
                if (instance.isTransparent)
                {
                    ++next.stats.transparentInstances;
                    continue;
                }
                if (!instance.pModel || instance.pModel->meshes.empty()) continue;
                if (!Finite(instance.translation) || !Finite(instance.rotation) || !Finite(instance.scale))
                {
                    m_lastError = "VXGI cannot build an instance with a non-finite transform.";
                    return false;
                }
                ++next.stats.includedInstances;
                if (instance.isCutout) ++next.stats.cutoutInstances;
                const auto world = DirectX::XMMatrixScaling(instance.scale.x, instance.scale.y, instance.scale.z) *
                    DirectX::XMMatrixRotationRollPitchYaw(instance.rotation.x, instance.rotation.y, instance.rotation.z) *
                    DirectX::XMMatrixTranslation(instance.translation.x, instance.translation.y, instance.translation.z);
                const auto normalMatrix = DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, world));
                HashWord(fingerprint, instance.editorId);
                for (size_t meshIndex = 0; meshIndex < instance.pModel->meshes.size(); ++meshIndex)
                {
                    const auto& mesh = instance.pModel->meshes[meshIndex];
                    if (mesh.lodIndices.empty()) continue;
                    const auto& indices = mesh.lodIndices[0];
                    const uint32_t materialId = instance.customMaterialID == UINT32_MAX ? mesh.materialID : instance.customMaterialID;
                    for (size_t i = 0; i + 2 < indices.size(); i += 3)
                    {
                        Triangle triangle = {};
                        triangle.instanceId = instance.editorId;
                        triangle.sourceInstanceIndex = static_cast<uint32_t>(sourceIndex);
                        triangle.meshIndex = static_cast<uint32_t>(meshIndex);
                        triangle.sourceTriangleIndex = static_cast<uint32_t>(i / 3);
                        triangle.materialId = materialId;
                        triangle.isCutout = instance.isCutout;
                        bool valid = true;
                        for (size_t vertex = 0; vertex < 3; ++vertex)
                        {
                            const uint32_t index = indices[i + vertex];
                            triangle.sourceVertexIndices[vertex] = index;
                            if (index >= mesh.vertices.size()) { valid = false; break; }
                            DirectX::XMStoreFloat3(&triangle.positions[vertex], DirectX::XMVector3TransformCoord(
                                DirectX::XMLoadFloat3(&mesh.vertices[index].Position), world));
                            const auto& sourceVertex = mesh.vertices[index];
                            if constexpr (requires { sourceVertex.TexCoords; sourceVertex.Normal; sourceVertex.Tangent; sourceVertex.Bitangent; })
                            {
                                triangle.uv[vertex] = sourceVertex.TexCoords;
                                // Match GBuffer VS: transform all three vectors by inverse-transpose,
                                // interpolate first, then orthonormalize at the hit. UVs are already flipped by import.
                                DirectX::XMStoreFloat3(&triangle.shadingNormals[vertex], DirectX::XMVector3TransformNormal(
                                    DirectX::XMLoadFloat3(&sourceVertex.Normal), normalMatrix));
                                DirectX::XMStoreFloat3(&triangle.tangents[vertex], DirectX::XMVector3TransformNormal(
                                    DirectX::XMLoadFloat3(&sourceVertex.Tangent), normalMatrix));
                                DirectX::XMStoreFloat3(&triangle.bitangents[vertex], DirectX::XMVector3TransformNormal(
                                    DirectX::XMLoadFloat3(&sourceVertex.Bitangent), normalMatrix));
                            }
                            if (!Finite(triangle.positions[vertex])) { valid = false; break; }
                        }
                        if (!valid) { ++next.stats.invalidTriangles; continue; }
                        const auto& p0 = triangle.positions[0];
                        const auto& p1 = triangle.positions[1];
                        const auto& p2 = triangle.positions[2];
                        const auto normal = Cross({double(p1.x)-p0.x, double(p1.y)-p0.y, double(p1.z)-p0.z},
                            {double(p2.x)-p0.x, double(p2.y)-p0.y, double(p2.z)-p0.z});
                        const double normalLength = std::sqrt(Dot(normal, normal));
                        if (normalLength == 0.0)
                        {
                            ++next.stats.degenerateTriangles;
                            continue;
                        }
                        triangle.geometricNormal = {float(normal.x/normalLength), float(normal.y/normalLength), float(normal.z/normalLength)};
                        if constexpr (!requires { mesh.vertices[0].Normal; mesh.vertices[0].TexCoords; mesh.vertices[0].Tangent; mesh.vertices[0].Bitangent; })
                            triangle.shadingNormals.fill(triangle.geometricNormal);
                        for (size_t vertex = 0; vertex < 3; ++vertex)
                        {
                            HashFloat3(fingerprint, triangle.positions[vertex]);
                            HashWord(fingerprint, triangle.sourceVertexIndices[vertex]);
                        }
                        HashWord(fingerprint, meshIndex);
                        HashWord(fingerprint, triangle.sourceTriangleIndex);
                        next.triangles.push_back(triangle);
                    }
                }
            }
            next.stats.triangles = next.triangles.size();
            next.stats.geometryFingerprint = fingerprint;
            next.stats.materialFingerprint = materialFingerprint;
            next.stats.coverageFingerprint = CoverageFingerprint(next.triangles);
            next.stats.geometryRebuilt = geometryChanged;
            if (!Allocate(next, voxelSize)) return false;
            ProbeAllocation nextProbes;
            if (!AllocateProbes(next.cells, voxelSize, probePaddingLayers, nextProbes)) return false;
            PublishProbes(next, std::move(nextProbes), probePaddingLayers);
            if(!m_initialized || next.stats.coverageFingerprint!=m_storage.stats.coverageFingerprint) ++m_coverageRevision;
            m_storage = std::move(next);
            m_snapshot = std::move(snapshot);
            m_voxelSize = voxelSize;
            m_initialized = true;
            m_invalidated = false;
            if (geometryChanged) ++m_geometryRevision;
            ++m_allocationRevision;
            if (materialChanged) ++m_materialRevision;
            ++m_revision;
            m_storage.stats.memoryBytes = EstimateMemoryBytes();
            m_storage.stats.buildMilliseconds = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            m_lastError.clear();
            return true;
        }
        catch (const std::exception& exception)
        {
            m_lastError = std::string("VXGI world allocation failed: ") + exception.what();
            return false;
        }
    }

    void Invalidate() { m_invalidated = true; }
    // These centers include the optional fixed-grid halo. Geometry accessors
    // deliberately retain the original triangle-occupied allocation.
    const std::vector<DirectX::XMFLOAT3>& GetOccupiedCenters() const { return m_storage.probeAllocation.centers; }
    const std::vector<Cell>& GetCells() const { return m_storage.cells; }
    const std::vector<Cell>& GetProbeCells() const { return m_storage.probeAllocation.cells; }
    const std::vector<Probe>& GetProbes() const { return m_storage.probeAllocation.probes; }
    const std::vector<Triangle>& GetTriangles() const { return m_storage.triangles; }
    const std::vector<uint32_t>& GetTriangleReferences() const { return m_storage.triangleReferences; }
    const Grid& GetGrid() const { return m_storage.grid; }
    const Grid& GetProbeGrid() const { return m_storage.probeAllocation.grid; }
    uint32_t GetProbePaddingLayers() const { return m_storage.stats.probePaddingLayers; }
    const Stats& GetStats() const { return m_storage.stats; }
    uint64_t GetRevision() const { return m_revision; }
    uint64_t GetGeometryRevision() const { return m_geometryRevision; }
    uint64_t GetMaterialRevision() const { return m_materialRevision; }
    uint64_t GetCoverageRevision() const { return m_coverageRevision; }
    uint64_t GetAllocationRevision() const { return m_allocationRevision; }
    float GetVoxelSize() const { return m_voxelSize; }
    const std::string& GetLastError() const { return m_lastError; }

    int32_t FindCell(Coordinate coordinate) const
    {
        const auto found = m_storage.lookup.find(coordinate);
        return found == m_storage.lookup.end() ? -1 : static_cast<int32_t>(found->second);
    }
    int32_t FindCell(const DirectX::XMFLOAT3& position) const
    {
        Coordinate coordinate;
        if (!m_initialized || !PositionCoordinate(position, m_voxelSize, coordinate)) return -1;
        return FindCell(coordinate);
    }
    int32_t FindProbeCell(Coordinate coordinate) const
    {
        const auto& lookup = m_storage.probeAllocation.lookup;
        const auto found = lookup.find(coordinate);
        return found == lookup.end() ? -1 : static_cast<int32_t>(found->second);
    }
    int32_t FindProbeCell(const DirectX::XMFLOAT3& position) const
    {
        Coordinate coordinate;
        if (!m_initialized || !PositionCoordinate(position, m_voxelSize, coordinate)) return -1;
        return FindProbeCell(coordinate);
    }

    // Thirteen-axis triangle/AABB SAT. Touching a cell boundary counts as overlap;
    // a triangle's bounding box is only a candidate generator, never occupancy.
    static bool TriangleIntersectsBox(const std::array<DirectX::XMFLOAT3, 3>& positions,
        const DirectX::XMFLOAT3& center, const DirectX::XMFLOAT3& halfExtent)
    {
        return IntersectsBox(positions, {center.x, center.y, center.z}, {halfExtent.x, halfExtent.y, halfExtent.z});
    }

private:
    static constexpr uint64_t HashSeed = 14695981039346656037ull;
    static constexpr uint64_t MaximumCandidateTests = 100000000ull;
    static constexpr uint64_t MaximumCells = 2000000ull;
    struct Double3 { double x, y, z; };
    struct CoordinateHash
    {
        size_t operator()(Coordinate coordinate) const
        {
            uint64_t hash = HashSeed;
            HashWord(hash, static_cast<uint32_t>(coordinate.x));
            HashWord(hash, static_cast<uint32_t>(coordinate.y));
            HashWord(hash, static_cast<uint32_t>(coordinate.z));
            return static_cast<size_t>(hash);
        }
    };
    struct ProbeAllocation
    {
        Grid grid;
        std::vector<Cell> cells; // Allocation only: no triangle CSR references.
        std::vector<Probe> probes;
        std::vector<DirectX::XMFLOAT3> centers;
        std::unordered_map<Coordinate, uint32_t, CoordinateHash> lookup;
    };
    struct Storage
    {
        Grid grid;
        Stats stats;
        std::vector<Triangle> triangles;
        std::vector<Cell> cells;
        ProbeAllocation probeAllocation;
        std::vector<uint32_t> triangleReferences;
        std::unordered_map<Coordinate, uint32_t, CoordinateHash> lookup;
    };
    struct Snapshot
    {
        uint32_t instanceId;
        uintptr_t model;
        std::array<uint32_t, 9> transform;
        uint32_t customMaterialId;
        bool transparent, cutout, hasGeometrySource;
        uint64_t geometryMetadata, materialMetadata;
        bool operator==(const Snapshot& other) const
        {
            return instanceId == other.instanceId && model == other.model && transform == other.transform &&
                customMaterialId == other.customMaterialId && transparent == other.transparent &&
                cutout == other.cutout && hasGeometrySource == other.hasGeometrySource &&
                geometryMetadata == other.geometryMetadata && materialMetadata == other.materialMetadata;
        }
    };

    static void HashWord(uint64_t& hash, uint64_t word)
    {
        for (unsigned byte = 0; byte < 8; ++byte) { hash ^= (word >> (byte * 8)) & 255; hash *= 1099511628211ull; }
    }
    static uint32_t FloatBits(float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }
    static void HashFloat3(uint64_t& hash, DirectX::XMFLOAT3 value)
    {
        HashWord(hash, FloatBits(value.x)); HashWord(hash, FloatBits(value.y)); HashWord(hash, FloatBits(value.z));
    }
    template<class MeshType>
    static void HashMaterial(uint64_t& hash, const MeshType& mesh, bool includeResourceIdentity)
    {
        HashWord(hash, mesh.materialID); HashWord(hash, mesh.isUnlit);
        HashWord(hash, FloatBits(mesh.baseColorFactor.x)); HashWord(hash, FloatBits(mesh.baseColorFactor.y));
        HashWord(hash, FloatBits(mesh.baseColorFactor.z)); HashWord(hash, FloatBits(mesh.baseColorFactor.w));
        HashFloat3(hash, mesh.emissiveFactor);
        for (const auto& texture : mesh.textures)
        {
            HashWord(hash, static_cast<uint32_t>(texture.type));
            if (includeResourceIdentity) HashWord(hash, reinterpret_cast<uintptr_t>(texture.Resource.Get()));
            for (unsigned char character : texture.path) HashWord(hash, character);
            HashWord(hash, 0);
        }
    }
    template<class InstanceRange>
    static std::vector<Snapshot> CaptureSnapshot(const InstanceRange& allInstances)
    {
        std::vector<Snapshot> snapshot;
        snapshot.reserve(allInstances.size());
        // Scan immutable-geometry metadata and editable material factors once per
        // unique model, rather than once per instance of an instanced asset.
        std::unordered_map<uintptr_t, std::pair<uint64_t,uint64_t>> modelMetadata;
        for (const auto& instance : allInstances)
        {
            Snapshot item = {};
            item.instanceId = instance.editorId;
            item.model = reinterpret_cast<uintptr_t>(instance.pModel);
            item.transform = { FloatBits(instance.translation.x), FloatBits(instance.translation.y), FloatBits(instance.translation.z),
                FloatBits(instance.rotation.x), FloatBits(instance.rotation.y), FloatBits(instance.rotation.z),
                FloatBits(instance.scale.x), FloatBits(instance.scale.y), FloatBits(instance.scale.z) };
            item.customMaterialId = instance.customMaterialID;
            item.transparent = instance.isTransparent; item.cutout = instance.isCutout;
            item.geometryMetadata = item.materialMetadata = HashSeed;
            item.hasGeometrySource = instance.pModel && !instance.pModel->meshes.empty();
            const auto existingMetadata = modelMetadata.find(item.model);
            if (existingMetadata != modelMetadata.end())
            {
                item.geometryMetadata = existingMetadata->second.first;
                item.materialMetadata = existingMetadata->second.second;
            }
            else if (instance.pModel)
            {
                HashWord(item.geometryMetadata, instance.pModel->meshes.size());
                for (const auto& mesh : instance.pModel->meshes)
                {
                    HashMaterial(item.materialMetadata, mesh, true);
                    HashWord(item.geometryMetadata, mesh.vertices.size());
                    HashWord(item.geometryMetadata, reinterpret_cast<uintptr_t>(mesh.vertices.data()));
                    HashWord(item.geometryMetadata, mesh.lodIndices.empty() ? 0 : mesh.lodIndices[0].size());
                    HashWord(item.geometryMetadata, mesh.lodIndices.empty() ? 0 : reinterpret_cast<uintptr_t>(mesh.lodIndices[0].data()));
                }
                modelMetadata.emplace(item.model, std::make_pair(item.geometryMetadata,item.materialMetadata));
            }
            snapshot.push_back(item);
        }
        // ResourceManager may reorder instances for batching. Source-index identity
        // is only dereferenced until another successful build, so preserve ordering
        // in the snapshot to refresh those indices after any reorder.
        return snapshot;
    }
    static std::vector<size_t> GeometryOrder(const std::vector<Snapshot>& snapshot)
    {
        std::vector<size_t> order;
        for (size_t i=0;i<snapshot.size();++i)
            if (!snapshot[i].transparent && snapshot[i].hasGeometrySource) order.push_back(i);
        std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b) {
            const auto& first=snapshot[a]; const auto& second=snapshot[b];
            if(first.instanceId!=second.instanceId) return first.instanceId<second.instanceId;
            if(first.model!=second.model) return first.model<second.model;
            if(first.transform!=second.transform) return first.transform<second.transform;
            return first.geometryMetadata<second.geometryMetadata;
        });
        return order;
    }
    static bool SameGeometry(const std::vector<Snapshot>& first,const std::vector<Snapshot>& second)
    {
        const auto a=GeometryOrder(first),b=GeometryOrder(second);
        if(a.size()!=b.size()) return false;
        for(size_t i=0;i<a.size();++i)
        {
            const auto& left=first[a[i]]; const auto& right=second[b[i]];
            if(left.instanceId!=right.instanceId || left.model!=right.model || left.transform!=right.transform ||
                left.geometryMetadata!=right.geometryMetadata) return false;
        }
        return true;
    }
    static uint64_t MaterialFingerprint(const std::vector<Snapshot>& snapshot)
    {
        const auto order=GeometryOrder(snapshot);
        uint64_t hash=HashSeed;
        for(size_t index:order)
        {
            const auto& item=snapshot[index];
            HashWord(hash,item.instanceId); HashWord(hash,item.customMaterialId); HashWord(hash,item.cutout);
            HashWord(hash,item.materialMetadata);
        }
        return hash;
    }
    static uint64_t CoverageFingerprint(const std::vector<Triangle>& triangles)
    {
        uint64_t hash=HashSeed;
        for(const auto& triangle:triangles)
        {
            HashWord(hash,triangle.instanceId);HashWord(hash,triangle.meshIndex);
            HashWord(hash,triangle.sourceTriangleIndex);HashWord(hash,triangle.materialId);
            HashWord(hash,triangle.isCutout);
        }
        return hash;
    }
    template<class InstanceRange>
    void RefreshMaterials(const InstanceRange& instances,const std::vector<Snapshot>& snapshot)
    {
        const auto oldOrder=GeometryOrder(m_snapshot),newOrder=GeometryOrder(snapshot);
        std::vector<size_t> remap(m_snapshot.size());
        for(size_t i=0;i<oldOrder.size();++i) remap[oldOrder[i]]=newOrder[i];
        for(auto& triangle:m_storage.triangles)
        {
            const size_t source=remap[triangle.sourceInstanceIndex];
            const auto& instance=instances[source];
            triangle.sourceInstanceIndex=static_cast<uint32_t>(source);
            triangle.materialId=instance.customMaterialID==UINT32_MAX ? instance.pModel->meshes[triangle.meshIndex].materialID : instance.customMaterialID;
            triangle.isCutout=instance.isCutout;
        }
        m_storage.stats.sceneInstances=instances.size();
        m_storage.stats.includedInstances=m_storage.stats.transparentInstances=m_storage.stats.cutoutInstances=0;
        for(const auto& instance:instances)
        {
            if(instance.isTransparent) ++m_storage.stats.transparentInstances;
            else if(instance.pModel && !instance.pModel->meshes.empty())
            {
                ++m_storage.stats.includedInstances;
                if(instance.isCutout) ++m_storage.stats.cutoutInstances;
            }
        }
    }
    static bool Finite(DirectX::XMFLOAT3 value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }
    static Double3 Subtract(Double3 a, Double3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
    static Double3 Cross(Double3 a, Double3 b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
    static double Dot(Double3 a, Double3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
    static bool IntersectsBox(const std::array<DirectX::XMFLOAT3, 3>& positions, Double3 center, Double3 half)
    {
        Double3 vertices[3];
        for (size_t i = 0; i < 3; ++i)
            vertices[i] = { double(positions[i].x) - center.x, double(positions[i].y) - center.y,
                double(positions[i].z) - center.z };
        const Double3 axes[3] = { {1,0,0}, {0,1,0}, {0,0,1} };
        const Double3 edges[3] = { Subtract(vertices[1], vertices[0]), Subtract(vertices[2], vertices[1]),
            Subtract(vertices[0], vertices[2]) };
        auto separated = [&](Double3 axis) {
            const double a = Dot(vertices[0], axis), b = Dot(vertices[1], axis), c = Dot(vertices[2], axis);
            const double radius = half.x * std::abs(axis.x) + half.y * std::abs(axis.y) + half.z * std::abs(axis.z);
            // Scale tolerance with the unnormalized axis projection. A fixed
            // absolute epsilon would incorrectly fill slivers' AABB corners.
            const double epsilon = 1e-9 * ((std::max)({std::abs(a), std::abs(b), std::abs(c)}) + radius);
            return (std::min)({a,b,c}) > radius + epsilon || (std::max)({a,b,c}) < -radius - epsilon;
        };
        for (auto axis : axes) if (separated(axis)) return false;
        if (separated(Cross(edges[0], edges[1]))) return false;
        for (auto edge : edges) for (auto axis : axes) if (separated(Cross(edge, axis))) return false;
        return true;
    }
    static bool ToCoordinate(double value, int32_t& output)
    {
        const double floored = std::floor(value);
        if (!std::isfinite(floored) || floored < INT32_MIN || floored > INT32_MAX) return false;
        output = static_cast<int32_t>(floored);
        return true;
    }
    static bool PositionCoordinate(DirectX::XMFLOAT3 position, float size, Coordinate& coordinate)
    {
        return ToCoordinate(double(position.x) / size, coordinate.x) && ToCoordinate(double(position.y) / size, coordinate.y) &&
            ToCoordinate(double(position.z) / size, coordinate.z);
    }
    static DirectX::XMFLOAT3 Center(Coordinate coordinate, float size)
    {
        return { float((double(coordinate.x) + 0.5) * size), float((double(coordinate.y) + 0.5) * size),
            float((double(coordinate.z) + 0.5) * size) };
    }
    static void ExtendGrid(Grid& grid, Coordinate coordinate)
    {
        if (!grid.valid)
        {
            grid.minimum = grid.maximum = coordinate;
            grid.valid = true;
            return;
        }
        grid.minimum.x = (std::min)(grid.minimum.x, coordinate.x);
        grid.minimum.y = (std::min)(grid.minimum.y, coordinate.y);
        grid.minimum.z = (std::min)(grid.minimum.z, coordinate.z);
        grid.maximum.x = (std::max)(grid.maximum.x, coordinate.x);
        grid.maximum.y = (std::max)(grid.maximum.y, coordinate.y);
        grid.maximum.z = (std::max)(grid.maximum.z, coordinate.z);
    }
    static void FinishGrid(Grid& grid, float size)
    {
        if (!grid.valid) return;
        grid.origin = {float(double(grid.minimum.x)*size), float(double(grid.minimum.y)*size), float(double(grid.minimum.z)*size)};
        grid.boundsMaximum = {float((double(grid.maximum.x)+1)*size), float((double(grid.maximum.y)+1)*size),
            float((double(grid.maximum.z)+1)*size)};
    }
    bool AllocateProbes(const std::vector<Cell>& geometryCells, float size, uint32_t paddingLayers, ProbeAllocation& next)
    {
        const int64_t padding = paddingLayers;
        // Full Chebyshev dilation, including edges and corners. Only coordinate
        // identity is expanded: halo cells never inherit triangle references.
        next.lookup.reserve((std::min)(MaximumCells, uint64_t(geometryCells.size()) * (2*paddingLayers+1)));
        for (const auto& cell : geometryCells)
        {
            const auto source = cell.coordinate;
            if (int64_t(source.x)-padding < INT32_MIN || int64_t(source.x)+padding > INT32_MAX ||
                int64_t(source.y)-padding < INT32_MIN || int64_t(source.y)+padding > INT32_MAX ||
                int64_t(source.z)-padding < INT32_MIN || int64_t(source.z)+padding > INT32_MAX)
            {
                m_lastError = "VXGI probe padding exceeds the supported grid coordinate range.";
                return false;
            }
            for (int64_t x = int64_t(source.x)-padding; x <= int64_t(source.x)+padding; ++x)
                for (int64_t y = int64_t(source.y)-padding; y <= int64_t(source.y)+padding; ++y)
                    for (int64_t z = int64_t(source.z)-padding; z <= int64_t(source.z)+padding; ++z)
                    {
                        next.lookup.emplace(Coordinate{static_cast<int32_t>(x),static_cast<int32_t>(y),static_cast<int32_t>(z)}, 0);
                        if (next.lookup.size() > MaximumCells)
                        {
                            m_lastError = "VXGI probe allocation exceeds two million cells; reduce probe padding or increase voxelSize.";
                            return false;
                        }
                    }
        }
        std::vector<Coordinate> coordinates;
        coordinates.reserve(next.lookup.size());
        for (const auto& entry : next.lookup) coordinates.push_back(entry.first);
        std::sort(coordinates.begin(), coordinates.end());
        next.cells.reserve(coordinates.size()); next.probes.reserve(coordinates.size()); next.centers.reserve(coordinates.size());
        for (const auto coordinate : coordinates)
        {
            const auto position = Center(coordinate, size);
            if (!Finite(position))
            {
                m_lastError = "VXGI probe padding produces a non-finite world center.";
                return false;
            }
            const auto index = static_cast<uint32_t>(next.cells.size());
            next.cells.push_back({coordinate,0,0,index});
            next.centers.push_back(position); next.probes.push_back({position});
            next.lookup.find(coordinate)->second = index;
            ExtendGrid(next.grid, coordinate);
        }
        FinishGrid(next.grid, size);
        return true;
    }
    static void PublishProbes(Storage& storage, ProbeAllocation&& probes, uint32_t paddingLayers)
    {
        storage.probeAllocation = std::move(probes);
        storage.stats.probeCells = storage.probeAllocation.cells.size();
        storage.stats.haloProbeCells = storage.stats.probeCells - storage.cells.size();
        storage.stats.probePaddingLayers = paddingLayers;
    }
    bool Allocate(Storage& next, float size)
    {
        std::unordered_map<Coordinate, std::vector<uint32_t>, CoordinateHash> candidates;
        const double gridTolerance = 1e-6;
        // Closed cells conservatively include both sides of an exact grid boundary.
        const double halfSize = double(size) * (0.5 + gridTolerance);
        const Double3 half = { halfSize, halfSize, halfSize };
        for (size_t index = 0; index < next.triangles.size(); ++index)
        {
            const auto& positions = next.triangles[index].positions;
            Coordinate minimum, maximum;
            const float minX = (std::min)({positions[0].x, positions[1].x, positions[2].x});
            const float minY = (std::min)({positions[0].y, positions[1].y, positions[2].y});
            const float minZ = (std::min)({positions[0].z, positions[1].z, positions[2].z});
            const float maxX = (std::max)({positions[0].x, positions[1].x, positions[2].x});
            const float maxY = (std::max)({positions[0].y, positions[1].y, positions[2].y});
            const float maxZ = (std::max)({positions[0].z, positions[1].z, positions[2].z});
            if (!ToCoordinate(double(minX)/size-gridTolerance, minimum.x) || !ToCoordinate(double(minY)/size-gridTolerance, minimum.y) ||
                !ToCoordinate(double(minZ)/size-gridTolerance, minimum.z) || !ToCoordinate(double(maxX)/size+gridTolerance, maximum.x) ||
                !ToCoordinate(double(maxY)/size+gridTolerance, maximum.y) || !ToCoordinate(double(maxZ)/size+gridTolerance, maximum.z))
            {
                m_lastError = "VXGI world coordinates exceed the supported grid range.";
                return false;
            }
            const double boxTests = (double(maximum.x)-minimum.x+1) * (double(maximum.y)-minimum.y+1) * (double(maximum.z)-minimum.z+1);
            if (boxTests > MaximumCandidateTests - next.stats.candidateBoxTests)
            {
                m_lastError = "VXGI allocation exceeds 100 million triangle/cell tests; increase voxelSize.";
                return false;
            }
            next.stats.candidateBoxTests += static_cast<uint64_t>(boxTests);
            for (int64_t x = minimum.x; x <= maximum.x; ++x)
                for (int64_t y = minimum.y; y <= maximum.y; ++y)
                    for (int64_t z = minimum.z; z <= maximum.z; ++z)
                    {
                        const Coordinate coordinate = {static_cast<int32_t>(x), static_cast<int32_t>(y), static_cast<int32_t>(z)};
                        const Double3 center = {(double(x)+0.5)*size, (double(y)+0.5)*size, (double(z)+0.5)*size};
                        if (!IntersectsBox(positions, center, half)) continue;
                        candidates[coordinate].push_back(static_cast<uint32_t>(index));
                        if (candidates.size() > MaximumCells)
                        {
                            m_lastError = "VXGI allocation exceeds two million occupied cells; increase voxelSize.";
                            return false;
                        }
                    }
        }
        std::vector<Coordinate> coordinates;
        coordinates.reserve(candidates.size());
        for (const auto& candidate : candidates) coordinates.push_back(candidate.first);
        std::sort(coordinates.begin(), coordinates.end());
        next.cells.reserve(coordinates.size());
        next.lookup.reserve(coordinates.size());
        for (const auto coordinate : coordinates)
        {
            const auto& references = candidates.find(coordinate)->second;
            if (next.triangleReferences.size() + references.size() > UINT32_MAX)
            {
                m_lastError = "VXGI triangle references exceed the supported compact index range.";
                return false;
            }
            const uint32_t cellIndex = static_cast<uint32_t>(next.cells.size());
            next.cells.push_back({ coordinate, static_cast<uint32_t>(next.triangleReferences.size()),
                static_cast<uint32_t>(references.size()), cellIndex });
            next.triangleReferences.insert(next.triangleReferences.end(), references.begin(), references.end());
            next.lookup.emplace(coordinate, cellIndex);
            ExtendGrid(next.grid, coordinate);
        }
        FinishGrid(next.grid, size);
        next.stats.cells = next.cells.size(); next.stats.triangleReferences = next.triangleReferences.size();
        return true;
    }
    uint64_t EstimateMemoryBytes() const
    {
        const auto& data = m_storage;
        return sizeof(*this) + data.triangles.capacity()*sizeof(Triangle) + data.cells.capacity()*sizeof(Cell) +
            data.probeAllocation.cells.capacity()*sizeof(Cell) + data.probeAllocation.probes.capacity()*sizeof(Probe) +
            data.probeAllocation.centers.capacity()*sizeof(DirectX::XMFLOAT3) +
            data.triangleReferences.capacity()*sizeof(uint32_t) + m_snapshot.capacity()*sizeof(Snapshot) +
            data.lookup.bucket_count()*sizeof(void*) + data.lookup.size()*(sizeof(decltype(data.lookup)::value_type)+2*sizeof(void*)) +
            data.probeAllocation.lookup.bucket_count()*sizeof(void*) +
            data.probeAllocation.lookup.size()*(sizeof(decltype(data.probeAllocation.lookup)::value_type)+2*sizeof(void*));
    }

    Storage m_storage;
    std::vector<Snapshot> m_snapshot;
    std::string m_lastError;
    float m_voxelSize = 0.0f;
    uint64_t m_revision = 0;
    uint64_t m_geometryRevision = 0;
    uint64_t m_materialRevision = 0;
    uint64_t m_coverageRevision = 0;
    uint64_t m_allocationRevision = 0;
    bool m_initialized = false;
    bool m_invalidated = true;
};
