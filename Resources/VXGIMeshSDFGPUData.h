#pragma once

#include "VXGIMeshSDFScene.h"
#include <array>
#include <cstddef>
#include <stdexcept>

// CPU/HLSL structured-buffer contract. Asset arrays retain their original local
// indices; the corresponding Asset record supplies each concatenation base.
class VXGIMeshSDFGPUData
{
public:
    struct Triangle
    {
        DirectX::XMFLOAT3 p0; uint32_t triangleId;
        DirectX::XMFLOAT3 p1; uint32_t flags;
        DirectX::XMFLOAT3 p2; uint32_t pad0; // Source mesh index, for instance materials.
        DirectX::XMFLOAT3 geometricNormal; uint32_t pad1;
        std::array<DirectX::XMFLOAT2,3> uv;
        std::array<DirectX::XMFLOAT3,3> shadingNormals,tangents,bitangents;
        uint32_t materialId,instanceId,pad2;
    };
    struct Asset
    {
        DirectX::XMFLOAT3 origin; float cellSize;
        std::array<uint32_t,3> cellDimensions; uint32_t nodeOffset;
        DirectX::XMFLOAT3 boundsMaximum; uint32_t cellOffset;
        uint32_t referenceOffset,triangleOffset,triangleCount,assetCount;
    };
    struct Instance
    {
        // DirectX row-vector matrices, consumed as row_major float4x4 in HLSL.
        DirectX::XMFLOAT4X4 worldFromLocal,localFromWorld,normalFromLocal;
        DirectX::XMFLOAT3 boundsMinimum; uint32_t assetIndex;
        DirectX::XMFLOAT3 boundsMaximum; uint32_t materialOffset;
        uint32_t materialCount,instanceId,flags,sceneInstanceCount;
    };
    static_assert(sizeof(Triangle)==208);
    static_assert(sizeof(Asset)==64 && offsetof(Asset,nodeOffset)==28 && offsetof(Asset,triangleOffset)==52);
    static_assert(sizeof(Instance)==240 && offsetof(Instance,boundsMinimum)==192 && offsetof(Instance,materialCount)==224);
    using AssetOwner = std::shared_ptr<const VXGIMeshSDFCache::Asset>;
    using AssetList = std::vector<AssetOwner>;
    struct Geometry
    {
        std::vector<float> nodes;
        std::vector<uint32_t> cellOffsets,references;
        std::vector<Triangle> triangles;
        std::vector<Asset> assets;
    };
    struct Instances
    {
        std::vector<Instance> instances;
        std::vector<uint32_t> materialIds;
    };
    using InstanceBVHNode = VXGIInstanceBVH::Node;
    static_assert(sizeof(InstanceBVHNode)==32 && offsetof(InstanceBVHNode,instanceIndex)==12 &&
        offsetof(InstanceBVHNode,escapeIndex)==28);
    struct InstanceAcceleration
    {
        std::vector<InstanceBVHNode> nodes;
        uint32_t nodeCount=0;
        uint64_t revision=0;
    };

    static InstanceAcceleration BuildInstanceAcceleration(const VXGIMeshSDFScene& scene)
    {
        const auto& bvh=scene.GetInstanceBVH();
        InstanceAcceleration result;
        result.nodeCount=Index(bvh.GetNodes().size());result.revision=bvh.GetRevision();
        result.nodes=bvh.GetNodes();
        // Root SRVs need a valid address even for an empty, count-gated query.
        // This padding is never traversed: nodeCount is the unpadded size.
        if(result.nodes.empty()) result.nodes.emplace_back();
        return result;
    }

    static AssetList CollectAssets(const VXGIMeshSDFScene& scene)
    {
        AssetList result;
        for(const auto& instance:scene.GetInstances())
            if(std::find(result.begin(),result.end(),instance.asset)==result.end())
                result.push_back(instance.asset);
        return result;
    }
    static Geometry BuildAssets(const AssetList& sources,bool accelerated=true)
    {
        Geometry result;
        for(const auto& owner:sources)
        {
            const auto& sdf=owner->GetSDF();const auto& grid=sdf.GetGrid();
            const auto& offsets=accelerated?sdf.GetGPUCellOffsets():sdf.GetCellOffsets();
            const auto& references=accelerated?sdf.GetGPUTraceReferences():sdf.GetTriangleReferences();
            Asset asset{};
            asset.origin=grid.origin;asset.cellSize=grid.cellSize;asset.cellDimensions=grid.cellDimensions;
            asset.boundsMaximum=grid.boundsMaximum;
            asset.nodeOffset=Index(result.nodes.size());asset.cellOffset=Index(result.cellOffsets.size());
            asset.referenceOffset=Index(result.references.size());asset.triangleOffset=Index(result.triangles.size());
            asset.triangleCount=Index(sdf.GetTriangles().size());asset.assetCount=Index(sources.size());
            Append(result.nodes,sdf.GetNodes());Append(result.cellOffsets,offsets);Append(result.references,references);
            for(size_t index=0;index<sdf.GetTriangles().size();++index)
            {
                const auto& source=sdf.GetTriangles()[index];Triangle target{};
                target.p0=source.positions[0];target.p1=source.positions[1];target.p2=source.positions[2];
                target.triangleId=Index(index);target.pad0=source.meshIndex;
                target.geometricNormal=source.geometricNormal;target.uv=source.uv;
                target.shadingNormals=source.shadingNormals;target.tangents=source.tangents;target.bitangents=source.bitangents;
                target.materialId=UINT32_MAX;
                result.triangles.push_back(target);
            }
            Index(result.triangles.size());result.assets.push_back(asset);
        }
        Index(result.assets.size());return result;
    }
    static Instances BuildInstances(const VXGIMeshSDFScene& scene,const AssetList& assets)
    {
        Instances result;
        result.instances.reserve(scene.GetInstances().size());
        for(const auto& source:scene.GetInstances())
        {
            const auto found=std::find(assets.begin(),assets.end(),source.asset);
            if(found==assets.end()) throw std::invalid_argument("Mesh SDF instance references an unpacked asset.");
            Instance instance{};
            instance.worldFromLocal=source.worldFromLocal;instance.localFromWorld=source.localFromWorld;
            instance.normalFromLocal=source.normalFromLocal;instance.boundsMinimum=source.boundsMinimum;
            instance.boundsMaximum=source.boundsMaximum;instance.assetIndex=Index(size_t(found-assets.begin()));
            instance.materialOffset=Index(result.materialIds.size());instance.materialCount=Index(source.materialIds.size());
            instance.instanceId=source.instanceId;instance.flags=source.isCutout?1u:0u;
            // Root SRVs contain only a GPU address. GetDimensions cannot report
            // their element count, so every nonempty instance table carries it.
            instance.sceneInstanceCount=Index(scene.GetInstances().size());
            Append(result.materialIds,source.materialIds);result.instances.push_back(instance);
        }
        Index(result.instances.size());return result;
    }
    static VXGISDF::Grid MakeEnvelope(const VXGIMeshSDFScene& scene,float cellSize)
    {
        VXGISDF::Grid result{};
        if(scene.GetInstances().empty()) return result;
        result.origin=scene.GetInstances().front().boundsMinimum;
        result.boundsMaximum=scene.GetInstances().front().boundsMaximum;
        for(const auto& instance:scene.GetInstances())
        {
            result.origin.x=(std::min)(result.origin.x,instance.boundsMinimum.x);
            result.origin.y=(std::min)(result.origin.y,instance.boundsMinimum.y);
            result.origin.z=(std::min)(result.origin.z,instance.boundsMinimum.z);
            result.boundsMaximum.x=(std::max)(result.boundsMaximum.x,instance.boundsMaximum.x);
            result.boundsMaximum.y=(std::max)(result.boundsMaximum.y,instance.boundsMaximum.y);
            result.boundsMaximum.z=(std::max)(result.boundsMaximum.z,instance.boundsMaximum.z);
        }
        // Only a world bounds envelope for existing pass constants/debug views.
        // Mesh tracing reads the actual local grid from its Asset record.
        result.cellSize=cellSize;result.cellDimensions={1,1,1};result.nodeDimensions={2,2,2};result.valid=true;
        return result;
    }
private:
    static uint32_t Index(size_t value)
    {
        if(value>UINT32_MAX) throw std::length_error("Mesh SDF GPU buffer exceeds 32-bit indexing.");
        return static_cast<uint32_t>(value);
    }
    template<class T> static void Append(std::vector<T>& destination,const std::vector<T>& source)
    {
        Index(destination.size()+source.size());destination.insert(destination.end(),source.begin(),source.end());
    }
};
